# Deneva 原生运行时验收记录

## 来源与范围

先对照 Deneva 实际源码再实现，具体源文件、SHA-256、机制映射和必要修正见 deneva-native-runtime-2026-11-04.md。本次实现原生 RDMA 事务状态、协程、预热/测量阶段隔离，以及沿用原矩阵入口。没有启动完整正式矩阵。

原四个锁接口不变。物理区间锁是唯一授权；状态表保存代次与年龄，wound 不偷锁，commit 与 wound 竞争同一 CAS。数据整批写完后统一释放。断连或不确定写入停止整轮，不继续生成有效性能结果。

## 已完成的正确性检查

- build-shared Release 编译通过；40 项 CTest 包括真实 RDMA 硬件用例均执行通过，无 skip。
- 同一 OS worker 内的多协程 FIFO 进展、独立栈/attachment、异常展开；最小 64 KiB 栈上的预热统计重置。
- 三策略真实 RDMA 测试：年龄跨重试保持、物理持锁者释放前不授权、wound 后持有者保留锁、迟到 wound 不命中新代次、100 次并发 wound/commit CAS 竞争、活跃客户端断连 poison。
- .66/.67 两个独立进程并发更新同一远端两记录范围，三策略各执行 400 次已提交增量；最终两记录都精确为 400，无私有副本、撕裂或丢失更新。
- 并发预热执行真实写事务；测量前锁集合为空，保留数据并重新截取流量；有限 trace 的测量逻辑事务数独立于预热；快照异常能够释放等待屏障并 join。
- 低 FD 预算回归：soft RLIMIT_NOFILE=128，同时建立 64 条独立 QP，关闭 63 条后剩余连接继续读取，再关闭全部重建。没有提高系统 ulimit。

早期通过的共享入口短验证：experiments/logs/smoke-not-formal-20261104-140314-668724，local 28 / cross-host 24 / capacity-preflight 6，报告入口也能解析绘图。这些是非正式运行验证，不作性能排名，不与旧数据合并。

最新硬件 CTest / 跨进程契约日志：experiments/logs/shared-check-20261104-141245-815021。
大并发完整几何预检：experiments/logs/smoke-not-formal-20261104-141324-864416，最大 32 OS worker × 4 coroutine、10000 key、5000 trace、10 ops、range 128，12/12 容量 case 通过；这是 2 秒预热/2 秒窗口的容量检查，不是正式性能数据。

当前核心二进制的跨主机短负载回归：experiments/logs/native-check-20261104-141657-016852，三策略 × 五协议 × YCSB-A/E/Payment/New Order = 60/60 通过。预热和测量阶段分别注入活跃数据故障：experiments/logs/native-check-20261104-142218-421637，均以 exit 3 失效且状态表拒绝后续新事务。

一小时本地/跨主机稳定性仍在进行，完成后补充；目前不把“整个移植已验收”标为完成。

## 发现并修正的问题

1. 把未获锁的中止和获锁后被 wound 混在一起，会调用非幂等的空集合 unlock_all。新增显式物理持锁标记，保留既有释放契约。
2. thread_local 索引 attachment、QP 指派和条件变量等待会把同 worker 的协程混为一体或阻塞其持有者。改为上下文局部 attachment、稳定 QP 指派和 FIFO cooperative yield。
3. 在协程栈上构造临时 Worker 会占约 150 KiB，超出合法最小栈；改为排空后就地重置。
4. 大并发时每 QP 重复打开设备/PD，超过普通 FD 上限。对照 Deneva Rdma::init 的共享 RNic 改为进程内共享设备/PD，不改变独立 CQ/MR/缓冲与断连销毁 QP 的边界。
5. 检查脚本运行中开发构建重链可导致执行权限错误。原生检查现在固定所有二进制并记录 SHA-256，避免跨 case 更换可执行文件；该次失败日志不算有效回归。
6. 原 local-only 检查误把第二个本地 peer 当成不存在的 remote 目录；已修复为两本地进程，--cross-host 仍为两主机。
7. resume 原先只按维度识别 case，可能混入旧提交/后端/预热设置。现拒绝不同源码、结果类别、协调后端、协程数、预热时长和栈大小，并校验全部解析配置指纹，覆盖 keys/ops/zipf/duration 等 CSV 原本缺失的输入。
8. none 基线报告 policy=none，但恢复时应归入首个 policy 的执行槽，否则会重复执行该基线。新增归一化键和回归测试。
9. destructive poison 测试必须最后使用其状态服务；CTest 显式安排共享数据用例先执行，不删除或跳过故障用例。

## 尚未消除的边界

当前 hierarchy 不是真实 B+ 树叶页层级锁；TPC-C 不是完整五事务。结构写 latch、全局年龄分配与单进程资源容量仍需正式数据评估。只移植此次运行负载所需机制，没有声称移植 Deneva 的分布式恢复、复制、持久化和所有业务模块。
