# RDMA 分片 gate 方案（下一阶段）

## 动机

上一阶段已经把整表读取降为 `active_slots` 前缀，但所有客户端仍竞争一个全局 gate。v5 的 next-key 区间和 v6 的逻辑 bucket 都有稳定的 key 范围，可以把锁表按逻辑 key bucket 分片，减少不相交请求之间的 CAS 冲突。

## 布局

`RemoteLockRegion` 由 16 个 `RemoteLockShard` 组成，每个 shard 有独立 gate、active slot count 和 64 个锁槽。总槽数仍为 1024，远程内存仍通过同一个 MR 暴露。区间映射使用可配置的逻辑 bucket 宽度；区间覆盖多个 bucket 时，在每个覆盖 shard 保存一份副本。

## 协议不变式

1. 加锁先按升序获取所有覆盖 shard 的 gate，再扫描和写入槽位；释放按逆序释放 gate，避免跨 bucket 死锁。
2. 区间副本只用于分片索引，冲突判断仍对完整 `KeyRangeEnt` 执行，因此不会放宽 next-key 或层级锁语义。
3. 同一逻辑锁可能在多个 shard 有副本；`unlock`/`unlock_all` 删除全部副本，`get_lock_count` 按区间和 kind 去重。
4. 客户端断开时服务端清理所有 shard 中该 owner 的副本。

## 已知边界

当前实现仍使用单条 RC QP 和进程内 I/O 互斥；分片首先降低远程 gate/槽表冲突和数据量，不能宣称已经实现多 QP 并行。跨主机 RoCE 和 per-thread QP 会在后续阶段单独测量。
