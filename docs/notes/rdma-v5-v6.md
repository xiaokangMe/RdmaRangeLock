# RDMA 环境与 v5/v6 接入笔记

## 环境

`.66` 上检测到 `mlx5_0`，端口处于 `PORT_ACTIVE`，链路层为 Ethernet（RoCE）；`libibverbs`、`librdmacm` 和开发头文件已安装。轻量实现直接使用 `libibverbs` 的 RC QP，TCP 仅用于交换 QP/GID/PSN 和远程内存地址、rkey。

## 远程锁表

`RemoteLockRegion` 包含一个 gate 和 1024 个 64 字节锁槽。客户端通过 RDMA compare-and-swap 抢占 gate，读取整个槽表，在本地执行与 M1 相同的 `KeyRangeEnt` overlap 和 `LockMode` 冲突判断，再用 RDMA WRITE 发布或清理槽位。这样保持 v5 Next-Key 的 `(prev,curr]` 语义和 v6 的 IS/IX/S/X 语义完全不变；区别只是锁表位于远程注册内存。

当前 gate 是全局 no-wait 元数据锁，用于先验证语义正确性。抢不到 gate 直接返回 `Abort`，不会自旋等待。RDMA CQ 轮询现在有 1 秒 deadline，TCP keepalive 检测断连；服务端检测到客户端断开后，会按 owner 清理 gate 和该 owner 的远程锁槽，避免异常退出污染下一轮实验。

当前一个 `RemoteLockManager` 使用单条 RC QP，并用进程内互斥保护该 QP，因此 RDMA smoke 是“单 QP 串行基线”，不能直接和本地多线程吞吐横比。正式性能实验需要后续增加 per-thread QP/连接池。

## 接入方式

- 服务端：`build-rdma/rrl_rdma_lock_server <port>`
- v5：`--cc rdma-next-key --rdma-host 127.0.0.1 --rdma-port <port>`
- v6：`--cc rdma-hierarchical --bucket 64 --rdma-host 127.0.0.1 --rdma-port <port>`
- 一键 smoke：`scripts/run_rdma_smoke.sh`

`run_next_key_lock` 和 `run_hierarchical_lock` 现在接受 `IRangeLockManager`，因此本地 `LockManager` 与 `RemoteLockManager` 共用同一事务协议和 workload，不需要复制 v5/v6 逻辑。

## 限制与下一步

当前实现是远程锁表基线：元数据更新由一个全局 gate 串行化，但槽表现在维护 `active_slots`，加锁只读取有效前缀，避免每次搬运完整 64 KiB。下一步应将 gate 按 bucket 分片，减少大范围扫描和多客户端之间的 RDMA 往返；同时增加 per-thread QP、跨主机 GID 配置和更细的故障恢复指标。
