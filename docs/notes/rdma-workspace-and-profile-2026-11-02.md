# RDMA gate 内缓冲复用、查询策略与测量方案

以 9ea3610 为基线处理后续 6–8。范围锁公共签名、远端布局、next-key 区间语义及 strict 2PL 不变；不启动完整矩阵。

## 6. 连接工作区与异常清理

每个 ConnectionSlot 私有持有 opaque Workspace，连接建立时预留 16 shard 的槽数组、释放搬移载荷、区间索引和有上界的请求数组。门闩 CAS 的 previous/requests、读请求、写分块都复用。传输层 WR/SGE 也预留 128 项，offset 使用固定数组。所有 reserve 在远端 gate 取得前完成，避免只移动表层分配、仍在 verbs 包装层分配。

本轮保留释放的 swap-with-last 协议及载荷快照，避免改 compaction 算法引入不确定写入。代价是每 QP 约数十 MiB 的本机工作区（按实际容量说明），不改变注册 MR 与远端容量。并发调用由原 io_mutex 保护工作区。

RemoteGateGuard 按作用域释放已取得的 gate；正常返回必须检查 release 结果，异常展开时不抛异常。gate 失败/槽表写失败保持既有对端恢复和 fail-closed 规则。事务缓存记忆、刷新和 count 去重移到 gate 释放后。C++ 异常转为 ERROR / count 错误，manager 标记不可继续使用并停止当前 QP，防止远端发布成功但本地缓存未建立时继续返回缓存命中。测试用私有 friend 注入确定性异常；不增加产品运行时注入开关。

## 7. 查询策略

保留 index 路径，增加 linear 路径供单变量对照：二者使用相同 RangeLock::conflicts 和 owned 匹配逻辑，不缓存跨请求的远端快照。SlotIndex 增加 reserve，仅构建当前加载的槽。可配置 RRL_SLOT_LOOKUP=index|linear，先保留当前默认值，测量包含 build + conflict + owned 的一次完整查询成本，禁止只计二分查找而忽略排序。

先通过差分正确性测试，再以有配置、源码提交标识和原始 CSV 的微基准比较三个重复；是否改变默认策略由结果决定，报告局限，不以微基准声称端到端收益。

## 8. 测量

RRL_RDMA_PROFILE=1 启用计时/流量统计，默认关闭。区分 WR 数与 post/poll 调用数（后者只作同步往返代理，不能等同线速 RTT），记录 read/write 提交字节、gate 持有时间、io_mutex 排队和本地 gate 等待、逻辑 lock 调用及缓存命中、查询策略检查量和索引构建成本。CSV 只追加列。配置、脚本说明如何固定连接数、线程、范围等变量，输出每事务代理往返和字节。

本轮完成测量工具与小规模对照，后续跨逻辑 next-key 请求的批处理仍须独立方案，不能合并区间改变锁粒度后直接比较协议。

## 6 的验证记录

工作区预留载荷约 25 MiB/QP（16×8192 的槽、搬移快照、四组 index 向量、写请求和冲突 ID），另有小量请求数组、容器及 allocator 开销；不计既有的 1 MiB 注册 scratch。连接数增加时需按 QP 线性预算本机内存。

异常捕获位于 io_mutex 保护范围内，先展开 RemoteGateGuard、释放本地 shard gate，再停止当前 QP/标记 manager 无效，最后解 io_mutex。已排队的调用重新检查 connected；不能再次命中失效缓存。分配失败发生在 TLS 分配连接之前也只标记 manager，清理不再尝试分配 TLS。正常释放校验失败同样标记失效。事务 cache/结果向量操作发生在远端和本地 shard gate 释放之后，io_mutex 仍保护该 QP 工作区。

新增真实 QP 回归用例：覆盖全 shard→单 shard→不同单 shard 缓冲复用、140 条锁的分块 compaction 与其他事务锁保留，以及 lock/release/count 三条路径在 gate 后抛 bad_alloc。全路径通过替换 C++ new/new[] 计数，持 gate 区间为零次（不据此推断 verbs 库内部 malloc）。异常后所有 gate 为零，manager 不可继续使用。

验证命令：cmake --build build-correctness-fix -j4；RRL_RDMA_GID_INDEX=3 python3 scripts/check_rdma_recovery.py --build build-correctness-fix。CTest 23/23 无跳过；8 个短负载、握手 deadline、恢复总预算及不确定修改 fail-closed 全通过。未跑完整矩阵或 1 小时压力。

## 7–8 工具与验收

RRL_SLOT_LOOKUP=index|linear（测量提交 befd2b2 默认 index，随后依数据改为 linear）；两路径共用 for_each_candidate、RangeLock::conflicts 与 find_owned 的精确匹配。随机差分包含 40,000 查询及主动 owned 命中。RRL_RDMA_PROFILE=0|1 默认 0，关闭时新增计时器不读时钟，新增计数保持 0；原超时检测仍照常读时钟。

CSV 追加 rdma_local_errors、profile 开关、linear 标志、逻辑请求/cache 命中、QP I/O 互斥等待、本地 gate 等待、远端 gate 持有时间、索引构建时间、冲突及 owned 查询时间、冲突候选检查量、同步提交次数及耗时、读写字节，以及 None/Gate/LoadCounts/LoadSlots/StoreSlot/ReleaseGate 六阶段调用/耗时。gate_hold 从全部 gate 成功取得后计至释放完成，不包含获取阶段，也不包含缓存记账；io_wait 包括 mutex 调用开销，不是 NIC send queue 深度。sync_calls 是实际 post_send 尝试次数（失败测量拒绝），sync_ns 从 post 前到 completion/失败处理结束；bytes 只计成功提交的 READ/WRITE 载荷，不含 CAS、协议包头。各项是累计总量，不是延迟分位数；并发累计等待不能除墙钟时间当作单线程占比。

真实 QP 计数回归：新锁、cache 命中、冲突、释放共 13 次同步提交 / 14 个 WR；六阶段 0/3/3/2/2/3 次，READ=152B、WRITE=80B；另验 profile 关闭仍可运行且新增统计为零。runner 把 local_errors 视为失败，硬件验收脚本同步拒绝该错误。

复现：
- python3 scripts/run_slot_lookup_bench.py --output experiments/results/slot-lookup-2026-11-02.csv
- RRL_RDMA_GID_INDEX=3 python3 scripts/run_rdma_profile.py --host SERVER_ROCE_IP --port ISOLATED_PORT --output experiments/results/rdma-lookup-profile-2026-11-02.csv

两个脚本要求源码/config/script 已提交、工作区干净，记录真实源码提交并构建对应目标，三次重复、交替策略顺序、拒绝覆盖已有文件。原始数据保持忽略，不进 Git。微基准使用 experiments/configs/slot-lookup-bench.json（8 种容量×5 种分布，完整 build+conflict+owned 成本）；next-key 用 experiments/configs/rdma-lookup-profile.json（固定 1,000 事务的小型诊断，不是正式性能矩阵）。后者只支持 YCSB-E/next-key，依赖独占且空的服务端；以全部 key 域作保守活跃槽界，4×4×(256+2)=4,128 < 8,192/shard，脚本拒绝超界。默认不启动服务，不扩大到跨机完整矩阵。

验证：默认 index/profile off 与 linear/profile on 各跑硬件 CTest 24/24 无跳过、8 个短负载及故障检查通过。尚未在此提交生成性能结论，先提交代码，再记录对应源码版本的数据。

最终默认策略和原始数据定位见 rdma-lookup-profile-results-2026-11-02.md。

默认策略切换后的最终验收：.66 硬件 CTest 24/24、8 个短负载及恢复/握手故障检查通过；.66→.67 的 loopback/workspace/profile 三个定向回归 3/3 通过。未启动完整矩阵。
