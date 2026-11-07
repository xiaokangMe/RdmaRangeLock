# RDMA record/gap 路径与容量修正方案（2026-11-07）

本次只修复代码并做正确性验证，不运行或续跑正式矩阵；既有结果不重标为新基线。

1. record：矩阵客户端显式传递对象锁开关；区间客户端在任何写入前探测带版本的区间表头，拒绝对象表及旧布局；复核正常和错配路径。
2. record/gap 范围读取：先枚举候选 key，再批量授权记录 S 锁，最后重新扫描取得锁后值；record 只消费原候选，保留其允许幻读的语义，gap 验证候选集合一致。严格保持提交/中止后统一释放。顺序批量回退取得部分锁后失败也必须释放，覆盖确定性回归。
3. 容量：服务端配置 rdma_lock_slots_per_shard，发布 magic/version/shards/capacity/stride/bytes；客户端据表头计算地址与预分配空间。默认 8192，配置可增大；大于单次传输缓冲区的槽快照分块读。恢复路径使用同一布局。增加测量窗口内每分片观察/发布占用峰值及容量失败时所需槽数，不把 lock_budget 当区间槽容量。参数进入配置指纹和续跑校验。

不改 IRangeLockManager 的公共协议签名，不改变索引页布局或引入第三方依赖。区间表的 wire layout 升级后须同时更新客户端和服务端，拒绝混用旧二进制。容量峰值是本客户端在 gate 内读取/发布的观察值；不声称为所有独立客户端的连续全局采样。

## 已实现

- 矩阵 lock_service_flags 从同一 case 配置生成客户端参数，record 显式收到 --native-object-locks；运行结果、续跑和报告均核对实际后端。
- 区间客户端用 owner=0 的只读探针验证表头，再建立带恢复身份的 QP。拒绝对象表、旧区间表、错误版本/stride/长度；显式指定容量时须与服务端公布值一致。校验仅在建链进行。
- record/gap 范围读取采用“枚举候选 → 批量记录 S 锁 → 一次复扫读取锁后值”。record 忽略新增未锁记录，候选删除导致中止；gap 校验候选集合。私有写可见性及严格释放保留，索引 latch 和事务锁仍各自独立。
- 原子区间批量授权在 gate 前预分配请求计划及发布载荷；非原子回退立即记录成功取得的前缀，中止/被 wound/到期均能释放。删除本地 native 授权后的重复 physical.lock。
- 区间槽使用带版本 header 和动态 stride；分片快照与恢复压缩均支持超过 1 MiB 的分块传输。
- 完整硬件测试揭示的旧兼容路径问题一并修复：TCP 协调器引擎和线协议均接受 ExactRange；远端区间/对象后端 unlock_all 空集合幂等成功，与本地 LockManager 一致。单独 unlock 不存在的范围仍返回 Abort，传输错误仍硬失败。

## 配置和后续补测

新候选：experiments/configs/formal_native_record_gap_capacity_2m_10s.json，**未运行**。不覆盖工作区已有未提交配置。

- 2,097,152 key，10 请求，10 秒预热/测量，3 次重复，标准 YCSB-A/E；
- record 走对象元数据；gap/next-key/桶层级走区间槽；
- rdma_lock_slots_per_shard=32768，16 分片；未配置时仍默认 8192；
- 真实枚举：本地 1368 case，跨主机 1296 case；
- lock_budget 与区间槽容量不同，不能仅增加前者解决后者。

矩阵仍先做容量预检，容量中止/gate 饱和使正式 case 无效。32768 是待验证候选，不保证极端热点下足够；总容量充足不代表最热分片充足。调容量后应开启新 campaign，不得混合续跑。容量也影响每个 QP 的预分配空间，增大时须检查内存资源。

单独区间服务命令：rrl_rdma_lock_server PORT --native-fail-stop --slots-per-shard 32768。

客户端 --rdma-lock-slots-per-shard 32768 声明预期，省略则发现实际容量；矩阵参数为 rdma_lock_slots_per_shard，环境入口为 RRL_RDMA_LOCK_SLOTS_PER_SHARD。

**客户端与服务端必须使用同一新版本。** 新旧 wire layout 不兼容，不能复用旧服务进程；矩阵会固定并分发同批二进制。旧结果属于旧 commit，本次未修改旧数据或旧状态文件。

## CSV 指标

- rdma_lock_slots_per_shard：协商的区间分片容量。本地/对象后端为 0，不代表对象后端没有持有者容量限制。
- rdma_lock_slot_peak_0 至 _15：测量窗口内本客户端在相应 gate 内观察/成功发布的最大占用，预热排空后清零。
- rdma_lock_capacity_required_0 至 _15：被拒容量请求在该分片的所需槽数峰值，仅记录触发拒绝的分片，0 表示该分片未记录拒绝，不代表一定有余量；并非所有在途请求的总需求预测。
- rows_rechecked 现在也包含 record/gap 锁后复扫；枚举/复扫分别计入 rows_scanned/rows_rechecked，不应与旧代码零复扫混比。

## 验证

构建：cmake --build build-shared -j4。

真实 RDMA 回环服务下完整 ctest --test-dir build-shared --output-on-failure：**47/47 通过，无跳过**。
日志：experiments/logs/record-gap-capacity-check-20261107-072414/ctest.log。

新增/扩展回归：

- 锁后值、加锁窗口删除；旧 own-write、幻读、释放用例均保留；
- 本地/native 状态下顺序批量第一把成功、第二把容量失败的跟踪与清理；
- gate 内批量授权无新增 C++ 动态分配；
- 8 槽边界及 32768 槽超过 1 MiB 快照、容量失败无部分发布、释放保留其他持有者、槽位复用、峰值清零；
- 对象服务误作区间服务时只读拒绝，随后正确对象客户端仍能执行；
- 使用矩阵同一参数生成函数，record/gap × 三种等待策略 × 两协程运行小规模事务，无容量/释放错误。

本次只有正确性验证，没有运行本地或跨主机正式补测，没有吞吐提升结论。
