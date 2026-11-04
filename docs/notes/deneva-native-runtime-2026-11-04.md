# Deneva 原生 RDMA 事务、协程与预热移植方案

目标：取消共享 RDMA 主实验的逐锁 TCP 协调请求，保留原有可运行路径作为显式兼容对照。遵守 CONSTRAINTS，所有协议继续共用索引和事务驱动，完整正式矩阵不自动启动。

## 已核对的 Deneva 实现

参考 /home/dell/pgm_deneva_backup：
- system/txn.cpp:2341..2563：RDMA_WAIT_DIE / RDMA_WOUND_WAIT 先 CAS 保护行元数据，读取 lock_owner/ts，比较年龄；旧事务等待，年轻事务中止或被 wound。数据通过 read_remote_row/write_remote_row 访问。
- concurrency_control/rdma_maat.h:47、rdma_maat.cpp:727..840：RdmaTxnTableNode 位于注册内存，按 worker/coroutine 找槽，记录 key 与 WOUND_RUNNING/COMMITTING/ABORTING；远端访问不经事务 RPC。
- system/txn.cpp:789：提交前从 RUNNING 转 COMMITTING；concurrency_control/rdma_2pl.cpp 的 finish 在完成写回后释放。
- system/worker_thread.cpp:540、671 与 system/txn.cpp:5619、6126：每 worker 多个栈式协程，独立事务/缓冲/QP，上报 RDMA 后 yield，在恢复时 poll CQ。
- system/sim_manager.cpp:104 与 system/helper.h:168：整轮并发预热，以时间门禁排除预热统计；配置 WARMUP_TIMER、DONE_TIMER 分离。
- benchmarks/ycsb_query.cpp、tpcc_query.cpp、tpcc_txn.cpp：查询生成与业务执行分离，TPC-C 当前生成器仅 Payment/New Order，不能称完整 TPC-C。

## 不能照搬的源代码问题

system/txn.cpp:2360、2406 附近存在 lock_type/lock_owner 使用 == 的无效表达式。rdma_maat.cpp:837 把状态变量地址以整个节点长度写回，可能越界读取并破坏邻接内存。不能把这些语句复制进新系统。

## 项目适配

1. 已有 RemoteLockManager 负责区间/节点锁、CAS shard gate 和冲突事务 ID；直接用它作为唯一锁授权，不再先向 TCP coordinator 重复授权。原四个锁接口签名不变。
2. 对应 Deneva RdmaTxnTableNode 增加被动 RDMA 事务状态服务：每会话/协程一个稳定槽，保存完整 attempt 代次和 age。用包含代次的单字 CAS 完成 ACTIVE->WOUNDED/COMMITTING，禁止迟到 wound 命中新 attempt。全局 age 由 RDMA 原子分配，重试保留 age，不比较不同机器时钟。
3. 等待保持持有者的物理锁，等待者只观察冲突者状态与重试真实锁授权；wound 不偷取 victim 的锁。数据写完->物理释放->状态结束。错误/不确定写入 poison 整轮，不自动恢复继续测量。
4. Deneva 式每 worker 多事务栈式协程，C++17 使用系统 ucontext 实现同类调度，避免新增 Boost 依赖。RDMA poll、短期 latch 等待、FIFO 资源准入必须可让出；所有 per-worker 数据状态改成 per-execution-context，不能用 thread_local 把同线程协程混为一个事务。
5. 每 case 以相同协议/并发运行 warm-up。预热后排空所有本轮在途事务并设阶段屏障，再统一清零统计、开始测量；保留预热写入，不在正式阶段重新灌数。计数、延迟和 RDMA 流量均按测量边界采集。
6. 配置增加协调后端、coroutines、warmup_seconds、栈大小；CSV 记录后端/并发事务数/预热与测量时间/状态流量/协程切换。历史结果类别和当前逻辑桶层级命名保留，不冒称真实叶页锁。
7. 正式入口保存这些参数，固定二进制、配套启动服务、容量门禁与绘图识别同步扩展；不以旧 TCP 结果作为新原生路径的单变量排名。

## 顺序与验收

先提交本方案；接口增量独立提交。随后实现直接 RDMA 策略并测试年龄、跨进程冲突、wound/commit 竞争、代次复用、故障与严格释放；再加入协程、预热、指标与矩阵兼容。每次代码变更执行 CTest。最终进行真实 .66/.67 定向负载、单线程多协程、预热排空、故障注入与一小时稳定性检查。保留其他 AI 的未跟踪图表脚本，禁止全局杀进程和 force push。

## 范围说明

当前 TPC-C 是简化操作 trace；运行时增强不等于完整移植 Deneva 业务事务，也不等于实现真实叶页层级。两者须分别设计验证。负载公平性、结构写串行和远端状态容量在报告中单列，不隐藏。
