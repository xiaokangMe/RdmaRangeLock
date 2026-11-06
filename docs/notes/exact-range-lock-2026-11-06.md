# 精确区间锁实现（2026-11-06）

本次新增独立协议名 exact-range / rdma-exact-range。它把一次范围查询的完整逻辑区间（包括开闭端点）作为一条 LockKind::ExactRange 锁发布；点读、更新、插入和删除也使用同一锁类型，因此区间内的点写会冲突，恰好位于半开上界的点写可以通过。锁在提交或回滚后统一释放。

实现路径：

* 本地：PolicyProtocol::ExactRange → LocalNativePolicyClient → LockManager。
* RDMA：rdma-exact-range → NativePolicyClient + RemoteLockManager，区间端点、锁模式和 ExactRange kind 都编码在远端槽中，沿现有 gate、批量读取、发布和释放路径运行。

Predicate 保留原有语义和命名，避免把两种实验结果混组。两者都保护完整谓词区间，但 ExactRange 使用独立的 kind、缓存键和槽记录，可单独测量锁表表示与冲突行为。未启动正式矩阵。
