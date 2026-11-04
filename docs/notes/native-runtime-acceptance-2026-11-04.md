# Deneva 原生运行时验收记录

## 来源与范围

先对照 Deneva 实际源码再实现，具体源文件、SHA-256、机制映射和必要修正见 deneva-native-runtime-2026-11-04.md。本次实现原生 RDMA 事务状态、协程、预热/测量阶段隔离，以及沿用原矩阵入口。没有启动完整正式矩阵。

原四个锁接口不变。物理区间锁是唯一授权；状态表保存代次与年龄，wound 不偷锁，commit 与 wound 竞争同一 CAS。数据整批写完后统一释放。断连或不确定写入停止整轮，不继续生成有效性能结果。

## 已完成的正确性检查

- build-shared Release 编译通过；41 项 CTest 包括真实 RDMA 硬件用例均执行通过，无 skip。
- 同一 OS worker 内的多协程 FIFO 进展、独立栈/attachment、异常展开；最小 64 KiB 栈上的预热统计重置。
- 三策略真实 RDMA 测试：年龄跨重试保持、物理持锁者释放前不授权、wound 后持有者保留锁、迟到 wound 不命中新代次、100 次并发 wound/commit CAS 竞争、活跃客户端断连 poison。
- .66/.67 两个独立进程并发更新同一远端两记录范围，三策略各执行 400 次已提交增量；最终两记录都精确为 400，无私有副本、撕裂或丢失更新。
- 并发预热执行真实写事务；测量前锁集合为空，保留数据并重新截取流量；有限 trace 的测量逻辑事务数独立于预热；快照异常能够释放等待屏障并 join。
- 低 FD 预算回归：soft RLIMIT_NOFILE=128，同时建立 64 条独立 QP，关闭 63 条后剩余连接继续读取，再关闭全部重建。没有提高系统 ulimit。

早期通过的共享入口短验证：experiments/logs/smoke-not-formal-20261104-140314-668724，local 28 / cross-host 24 / capacity-preflight 6，报告入口也能解析绘图。这些是非正式运行验证，不作性能排名，不与旧数据合并。

最新硬件 CTest / 跨进程契约日志：experiments/logs/shared-check-20261104-150835-236943（41/41，含隔离服务的物理锁 QP 故障回归）。
大并发完整几何预检：experiments/logs/smoke-not-formal-20261104-141324-864416，最大 32 OS worker × 4 coroutine、10000 key、5000 trace、10 ops、range 128，12/12 容量 case 通过；这是 2 秒预热/2 秒窗口的容量检查，不是正式性能数据。

当前核心二进制的跨主机短负载回归：experiments/logs/native-check-20261104-141657-016852，三策略 × 五协议 × YCSB-A/E/Payment/New Order = 60/60 通过。预热和测量阶段分别注入活跃数据故障：experiments/logs/native-check-20261104-142218-421637，均以 exit 3 失效且状态表拒绝后续新事务。

跨主机一小时正常路径检查已通过：experiments/logs/native-check-20261104-141518-354787，固定 135ef9e 核心二进制、next-key/YCSB-E/wound-wait、2 worker × 2 coroutine、1 秒预热、3600 秒窗口；硬错误/超时/释放失败/容量和 gate 中止为零，末尾树校验通过。本地同版本 BTreeLatch/hierarchy/YCSB-A/wound-wait、4 worker × 4 coroutine 的 3600 秒检查也已通过；日志 local-endurance.stdout/.stderr、local-endurance-validated.json 在同目录，硬错误/容量/gate 中止为零，末尾树校验通过。该版本不含后来加入的 YCSB 默认去重、直接 seed 构造及原生物理服务 fail-stop，不将旧版本长测冒称最新版全覆盖。

## 发现并修正的问题

1. 把未获锁的中止和获锁后被 wound 混在一起，会调用非幂等的空集合 unlock_all。新增显式物理持锁标记，保留既有释放契约。
2. thread_local 索引 attachment、QP 指派和条件变量等待会把同 worker 的协程混为一体或阻塞其持有者。改为上下文局部 attachment、稳定 QP 指派和 FIFO cooperative yield。
3. 在协程栈上构造临时 Worker 会占约 150 KiB，超出合法最小栈；改为排空后就地重置。
4. 大并发时每 QP 重复打开设备/PD，超过普通 FD 上限。对照 Deneva Rdma::init 的共享 RNic 改为进程内共享设备/PD，不改变独立 CQ/MR/缓冲与断连销毁 QP 的边界。
5. 检查脚本运行中开发构建重链可导致执行权限错误。原生检查现在固定所有二进制并记录 SHA-256，避免跨 case 更换可执行文件；该次失败日志不算有效回归。
6. 原 local-only 检查误把第二个本地 peer 当成不存在的 remote 目录；已修复为两本地进程，--cross-host 仍为两主机。
7. resume 原先只按维度识别 case，可能混入旧提交/后端/预热设置。现拒绝不同源码、结果类别、协调后端、协程数、预热时长和栈大小，并校验全部解析配置指纹，覆盖 keys/ops/zipf/duration 等 CSV 原本缺失的输入。
8. none 基线报告 policy=none，但恢复时应归入首个 policy 的执行槽，否则会重复执行该基线。新增归一化键和回归测试。
9. 全部 case 都已完成的 resume 原先不写新 CSV，却发布 complete；现先原子保存保留行，并拒绝重复/意外 case、空协议矩阵。子进程回归确保不重跑 case、CSV 实际存在且报告可读。
10. destructive poison 测试必须最后使用其状态服务；CTest 显式安排共享数据用例先执行，不删除或跳过故障用例。

## 尚未消除的边界

当前 hierarchy 不是真实 B+ 树叶页层级锁；TPC-C 不是完整五事务。结构写 latch、全局年龄分配与单进程资源容量仍需正式数据评估。只移植此次运行负载所需机制，没有声称移植 Deneva 的分布式恢复、复制、持久化和所有业务模块。


## 本轮新增验收

- YCSB 可配置请求数、默认事务内起始键唯一、非法容量拒绝、固定种子重现和显式重复键模式通过；重复键读己之写及容量失败回归仍完整保留。
- 直接构造远端初始记录集合，与旧本地 seed_index 装载器在 A/B/E/Payment/New Order、两种采样模式下逐 key/value 比较完全一致，避免大规模装载前构造无用临时 B+ 树。
- 原生锁连接故障回归：活跃/COMMITTING/持 gate 时，单独让 lock QP 发生真实 failed WC，同时保持 data/state QP 活着；竞争者不得获锁，物理服务 exit 2。正常释放后断连不停止服务。CTest 的隔离子服务不会破坏原共享 fixture。
- 双机原生短检查 native-check-20261104-145952-926823：六个数据/物理故障用例通过，三策略 × 五协议 × 四负载 60/60 通过。数据故障后可能残留物理锁，锁服务按 fail-stop 退出是预期失败封闭，不要求它重新开放授权。
- 默认采样配置经正式入口的非正式验证完成：experiments/native-deneva-entry-check/logs/smoke-not-formal-20261104-150327-782664，本地 28/28、跨主机 24/24、预检 6/6。原始产物不入库，也不是正式性能数据。
- 两百万 key 域、十秒预热/十秒测量：experiments/logs/deneva-2m-validation/logs/smoke-not-formal-20261104-150852-833227，本地 4/4、跨主机 4/4、预检 2/2；A 实际初始行 2097152，E 为 2096130。大并发容量检查另行记录，不把小维度验证等同整个正式矩阵。

TCP 用于 QP/MR 建链、连接存活和故障/断连通知；原生热路径不经逐事务或逐锁 TCP 授权。当前 gate 仍是动态区间槽的 shard gate，没有冒称已经换成 Deneva 的真实叶节点 intent_lock。源码核查见 deneva-gate-scale-warmup-2026-11-04.md。

两份 smoke 结果的报告脚本已校验、绘图成功：experiments/logs/native-report-entry-20261104/report.md 与 native-report-2m-20261104/report.md，明确为非正式验证产物，不入库、不作排名。

最大几何容量预检已完成：experiments/logs/deneva-2m-capacity-validation/logs/smoke-not-formal-20261104-151227-047528，12/12。两百万 key 域、32 worker × 4 coroutine（128 上下文）、10 请求、range128、三策略、hierarchy bucket16/64/256；每点十秒预热+十秒测量，均有提交，容量/gate 中止与硬错误为零。只执行预检，没有启动该配置的完整正式矩阵。
