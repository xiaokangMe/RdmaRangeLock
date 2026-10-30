# 代码审阅修复记录（2026-10-31）

本次修复对应 `docs/notes/code-review-2026-10-30.md`：

- RDMA CQ 轮询增加 1 秒 deadline；TCP keepalive 和服务端断连清理避免 gate 与 owner 锁槽永久残留。
- 远程锁表增加 `active_slots`，只读取有效槽前缀；`get_lock_count` 与其它操作一样经过 gate。
- `leave_gate` 改为 owner CAS；服务端按 owner 清理断开客户端的 gate 和锁槽。
- RDMA 设备选择支持 `RRL_RDMA_DEVICE`，GID index 支持 `RRL_RDMA_GID_INDEX`，并校验端口为 `PORT_ACTIVE`。
- `unlock` 增加可选 `LockKind`，Record 与 Gap 同一区间可以独立释放；父意向锁使用统一 `is_unbounded()` 判定。
- 统计拆出 `rows_rechecked`，吞吐同时输出 committed/s 与 attempted/s；runner 增加规范 CSV 输出。
- v5/v6 测试加入索引扫描、线程同步的端到端 phantom 场景；RDMA smoke 脚本自动配置和构建 `build-rdma`。

仍保留为下一阶段的结构性工作：per-thread QP/连接池、按 bucket 分片 gate、直接注册调用方 buffer，以及完整等待队列语义。当前 RDMA 性能数字明确标注为单 QP no-wait 基线。
