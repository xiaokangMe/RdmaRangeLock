# RDMA 环境与 v5/v6 接入笔记

## 环境

`.66` 上检测到 `mlx5_0`，端口处于 `PORT_ACTIVE`，链路层为 Ethernet（RoCE）；`libibverbs`、`librdmacm` 和开发头文件已安装。轻量实现直接使用 `libibverbs` 的 RC QP，TCP 仅用于交换 QP/GID/PSN 和远程内存地址、rkey。

## 远程锁表

`RemoteLockRegion` 包含 16 个 `RemoteLockShard`。每个 shard 有独立 gate、有效槽计数和 64 个 64 字节锁槽，总容量仍为 1024 个槽。客户端根据逻辑 bucket 映射请求涉及的 shard；跨 bucket 的区间在每个涉及 shard 保存一份副本。客户端通过 RDMA compare-and-swap 按 shard ID 升序抢 gate，读取有效槽前缀，在本地执行与 M1 相同的 `KeyRangeEnt` overlap 和 `LockMode` 冲突判断，再用 RDMA WRITE 发布或清理槽位。这样保持 v5 Next-Key 的 `(prev,curr]` 语义和 v6 的 IS/IX/S/X 语义完全不变；区别只是锁表位于远程注册内存。

当前各 shard gate 是 no-wait 元数据锁，用于保证每个分片的槽表一致性。抢不到任一 gate 直接返回 `Abort`，不会自旋等待；多 shard 请求始终按升序获取、逆序释放，避免跨 bucket 死锁。RDMA CQ 轮询现在有 1 秒 deadline，TCP keepalive 检测断连；服务端检测到客户端断开后，会遍历所有 shard 清理 gate 和该 owner 的远程锁槽，避免异常退出污染下一轮实验。

当前一个 `RemoteLockManager` 使用单条 RC QP，并用进程内互斥保护该 QP，因此 RDMA smoke 是“单 QP 串行基线”，不能直接和本地多线程吞吐横比。正式性能实验需要后续增加 per-thread QP/连接池。

## 接入方式

- 服务端：`build-rdma/rrl_rdma_lock_server <port>`
- v5：`--cc rdma-next-key --rdma-host 127.0.0.1 --rdma-port <port>`
- v6：`--cc rdma-hierarchical --bucket 64 --rdma-host 127.0.0.1 --rdma-port <port>`
- 一键 smoke：`scripts/run_rdma_smoke.sh`

`run_next_key_lock` 和 `run_hierarchical_lock` 现在接受 `IRangeLockManager`，因此本地 `LockManager` 与 `RemoteLockManager` 共用同一事务协议和 workload，不需要复制 v5/v6 逻辑。

## 限制与下一步

当前实现是远程锁表基线：每个 shard 维护 `active_slots`，加锁只读取有效前缀；分片 gate 已完成，减少了不相交 bucket 之间的 gate 冲突。下一步增加 per-thread QP、跨主机 GID 配置和更细的故障恢复指标。
