# Deneva 原生 RDMA 事务、协程与预热移植方案

目标：取消共享 RDMA 主实验的逐锁 TCP 协调请求，保留原有可运行路径作为显式兼容对照。遵守 CONSTRAINTS，所有协议继续共用索引和事务驱动，完整正式矩阵不自动启动。

## 已核对的 Deneva 实现

参考 /home/dell/pgm_deneva_backup：
- system/txn.cpp:2341..2563：RDMA_WAIT_DIE / RDMA_WOUND_WAIT 先 CAS 保护行元数据，读取 lock_owner/ts，比较年龄；旧事务等待，年轻事务中止或被 wound。数据通过 read_remote_row/write_remote_row 访问。
- concurrency_control/rdma_maat.h:47、rdma_maat.cpp:727..840：RdmaTxnTableNode 位于注册内存，按 worker/coroutine 找槽，记录 key 与 WOUND_RUNNING/COMMITTING/ABORTING；远端访问不经事务 RPC。
- system/txn.cpp:789：提交前从 RUNNING 转 COMMITTING；concurrency_control/rdma_2pl.cpp 的 finish 对写记录执行 write_and_unlock；但该分支先释放读集合再处理写集合，不能误称为全事务先写完再释放。我们的统一驱动保留更严格的整批数据写完后统一释放。
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


## 精确来源与移植差异

Deneva 目录的 git 基线为 a1a2f46，工作树含已有修改；以下以实际读取文件的 SHA-256 标识，而不是声称来自干净 upstream。此次未修改 Deneva。

- system/worker_thread.cpp:392 的 txn ID 编码包含 node/thread/coroutine 和轮次。本实现对应为 session slot + generation；session slot 在启动时通过 NIC CAS 领取，以支持独立进程无需静态 node 编号。
- system/manager.cpp:39 支持 TS_MUTEX/TS_CAS/TS_CLOCK，当前 config.h:316 是 TS_CLOCK。我们选择其原子分配思路，用共享 RDMA CAS 分配逻辑事务年龄，重试继承，避免依赖跨机器时钟。全局年龄字是潜在性能热点，尚未宣称消除；批量年龄分配尚未启用。
- system/worker_thread.cpp:671、682 是每 worker 的 master/transaction 协程。本实现以系统 ucontext 实现同类栈式轮转，不引入 Deneva 的 Boost 依赖；每个执行上下文独立索引 attachment、事务状态 QP、数据 QP 和缓冲，物理锁 QP 支持显式数量覆盖与 FIFO 共享。
- system/txn.cpp:5619、6126 的 READ/CAS post 后 yield、恢复后 poll CQ，对应现有 verbs post/poll 内的 cooperative_yield。不会把多个 OS 线程冒充协程。
- concurrency_control/rdma_maat.cpp:737..840 是注册内存状态表。我们把完整 attempt ID 与状态放在一个 64 位字里 CAS，状态写入严格为 8 字节；不是复制其错误的整节点长度写入。
- system/txn.cpp:2341..2563 的持有者年龄判断、旧等待/年轻中止、旧 wound 年轻，对应 NativePolicyClient::acquire。范围/层级的实际冲突集合仍由项目的物理区间锁表提供；无第二套 TCP 元数据授权。
- Deneva 有些路径对本机注册内存使用 CPU 写/原子；本实现的被动状态服务注册后只由 NIC 访问。断连检查也经恢复 QP READ/CAS，不假设 CPU 原子与 NIC CAS 互斥。
- system/sim_manager.cpp:104、system/helper.h:168 的时间门禁对应每 case 全 worker 并发预热。我们另加排空/屏障，防止跨预热边界的事务混入延迟或流量，保留预热后的数据。
- Deneva 原有 abort_queue 的延时惩罚没有搬来；按本仓库 CONSTRAINTS 禁止 sleep/backoff 的约束，已中止且释放的事务让出调度后保留 age 重试，截止时间与尝试上限继续有效。

实际源码摘要：

- system/worker_thread.cpp: 6405e6188fd095f0f9e19c2e99651c3e891356c91cca5526a5d1a659be112cc7
- system/txn.cpp: 6938edda8d891e37003c6726fc8ed9d8708541c838551b7bbe286e32916c2710
- system/routine.h: 18ab3602591786fd21a64340d10b14810383d2a44a4c0d54df3c32bb2e537fb9
- system/thread.cpp: 5b397f16039a8437147a2d725ae3cd8ddfebf6dce6b3435a777d492eedf1e88b
- system/helper.h: 2f705236f7cc819313df559a9b6cd806184b130afb969a441393b766e4501825
- system/sim_manager.cpp: e296cfa9d5e7f32c52e92163a3386c34175affe0a385f4b57d1c6b2bd65b90c4
- system/manager.cpp: a53670f4076a99c11eb273a87365031b451f79d08cbeb0ee5f027ae35349c992
- concurrency_control/rdma_maat.h: f5eff1e9a944473b310b314d58f14b0cb7ed99b078db8a1e2da19195bd77d6fd
- concurrency_control/rdma_maat.cpp: e17a7047f12b3c717c19156a2af49bd0f3b397a178d3f3cd1c886da83e5236c2
- concurrency_control/rdma_2pl.cpp: 3ae8c2c0b3141a0722fed551c3aac9fbaf78e99a395993da4386dc18e7392c4f
- concurrency_control/row_rdma_2pl.cpp: 9fee29a948c613f033f61a84a59ed918c25d1afbd2fa4c74b4146b09a4702e46
- config.h: afda7ac2729b3960fc9f8e82f6b0b711ea5b85f5d556fc8c882390aa4296ca20


## 大并发建链的源代码对照与修复计划

32 worker × 4 coroutine 的完整几何预检在事务状态 probe 建链失败。两台主机的 soft RLIMIT_NOFILE 都为 1024。当前每个 RdmaConnection 都 ibv_open_device + ibv_alloc_pd，三类后端按上下文分配 QP 后会重复消耗设备 FD，不能通过提高 ulimit 掩盖。

Deneva transport/rdma.cpp:446..464 的 Rdma::init 只创建一个 RNic，再创建 server/client RegHandler；client_qp 为 worker/coroutine 创建 RC QP 时使用共享 nic。按此方式，把本项目的设备 context/PD 做成进程内按设备选择共享的引用计数资源，保留每个 QP 独立 CQ、scratch MR 和服务端 region MR。最后一个 QP/MR 销毁后才释放 PD/context；fork 子进程不得复用父进程 pool。原来的断连先销毁 QP、再恢复内存的顺序不变，不共享 CQ 或可写 scratch。

验证：低 FD 上限下同时创建 64 个真实 QP，关闭其中 63 个后剩余 QP 可继续读；重跑硬件 CTest、完整几何容量预检和跨主机短负载。Deneva 单一共享 MR 尚不直接搬来，因为现有服务端 MR 的连接生命周期与故障隔离需要分别保持，不在此次混改。


## 原生物理锁连接断开：失败封闭方案

复查发现旧 clear_owner_state 会清除断开 QP 的物理锁。取消 TCP 第二授权后，这不能用于原生运行时：若事务已 COMMITTING、其数据 QP 仍可用，锁 QP 断开后清锁会放入竞争写事务，导致旧写覆盖。

增加显式 --native-fail-stop 模式：恢复用 NIC CAS 检查所有 shard；发现断开 owner 持有 gate 或 slot，停止整个物理服务，决不清理再授权。无残留锁的正常断开不影响服务。旧兼容模式保留原恢复契约。原生 SharedServices 必须启动此模式，专门故障测试保持数据/状态连接活着、让 COMMITTING 事务的物理 QP 消失，再验证竞争者不得获锁。该策略是实验失败封闭，不是完整故障恢复；已有一小时运行仅覆盖此前版本正常路径，版本单独记录。
