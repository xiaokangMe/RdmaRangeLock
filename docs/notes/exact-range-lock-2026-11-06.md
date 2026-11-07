# 精确区间锁实现（2026-11-06）

本次新增独立协议名 exact-range / rdma-exact-range。它把一次范围查询的完整逻辑区间（包括开闭端点）作为一条 LockKind::ExactRange 锁发布；点读、更新、插入和删除也使用同一锁类型，因此区间内的点写会冲突，恰好位于半开上界的点写可以通过。锁在提交或回滚后统一释放。

实现路径：

* 本地：PolicyProtocol::ExactRange → LocalNativePolicyClient → LockManager。
* RDMA：rdma-exact-range → NativePolicyClient + RemoteLockManager，区间端点、锁模式和 ExactRange kind 都编码在远端槽中，沿现有 gate、批量读取、发布和释放路径运行。

2026-11-07 复现审计补充：Predicate 与 ExactRange 的区间、模式、冲突和释放相同，只有 kind/cache 身份不同，没有独立表示优化。两者是 Eswaran1976 §3 一维区间谓词锁机制的兼容入口，综述必须合并计数，旧数据保留原标签以便追溯，不能解释成两种论文算法。详见 [复现审计](paper-reproduction-audit-2026-11-07.md)。本笔记创建时未启动正式矩阵；后续数据应按各自 commit 另查，不能由此推断当前实验数量。
