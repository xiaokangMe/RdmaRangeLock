# RDMA 环境与 v5/v6 接入笔记

## 环境

`.66` 上检测到 `mlx5_0`，端口处于 `PORT_ACTIVE`，链路层为 Ethernet（RoCE）；`libibverbs`、`librdmacm` 和开发头文件已安装。轻量实现直接使用 `libibverbs` 的 RC QP，TCP 仅用于交换 QP/GID/PSN 和远程内存地址、rkey。

## 远程锁表

`RemoteLockRegion` 包含一个 gate 和 1024 个 64 字节锁槽。客户端通过 RDMA compare-and-swap 抢占 gate，读取整个槽表，在本地执行与 M1 相同的 `KeyRangeEnt` overlap 和 `LockMode` 冲突判断，再用 RDMA WRITE 发布或清理槽位。这样保持 v5 Next-Key 的 `(prev,curr]` 语义和 v6 的 IS/IX/S/X 语义完全不变；区别只是锁表位于远程注册内存。

当前 gate 是全局 no-wait 元数据锁，用于先验证语义正确性。抢不到 gate 直接返回 `Abort`，不会自旋等待；客户端异常退出时 gate 可能遗留，需要后续增加租约或服务端故障恢复。

## 接入方式

- 服务端：`build-rdma/rrl_rdma_lock_server <port>`
- v5：`--cc rdma-next-key --rdma-host 127.0.0.1 --rdma-port <port>`
- v6：`--cc rdma-hierarchical --bucket 64 --rdma-host 127.0.0.1 --rdma-port <port>`
- 一键 smoke：`scripts/run_rdma_smoke.sh`

`run_next_key_lock` 和 `run_hierarchical_lock` 现在接受 `IRangeLockManager`，因此本地 `LockManager` 与 `RemoteLockManager` 共用同一事务协议和 workload，不需要复制 v5/v6 逻辑。

## 限制与下一步

当前实现是远程锁表基线：每次申请会读取槽表，元数据更新由一个全局 gate 串行化。后续应将 gate 拆成分片或按 bucket 的 CAS，减少大范围扫描和多客户端之间的 RDMA 往返；同时增加 server-side lease、断连清理和跨主机 GID 配置。