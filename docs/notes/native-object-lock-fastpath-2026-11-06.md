# Native object lock fast path（2026-11-06）

## 目的

普通 `rdma-record-2pl` 之前复用 `RemoteLockManager` 的 shard gate、活动槽前缀读取和冲突扫描。该路径仍保留范围语义，但精确记录锁并不需要区间相交扫描。

本阶段增加显式配置 `native_object_locks=1`（或 `--native-object-locks 1`），仅适用于 `rdma-record-2pl + index=rdma + txn_state_port`。它把物理记录锁切换到已有的 Deneva-style `ObjectLockManager`：

```text
远端注册对象目录
→ 对象 mutex 的 RDMA CAS
→ 读取 holder 链并做本地模式判断
→ 写入 holder / head
→ CAS 释放对象 mutex
```

事务状态仍由 native RDMA 状态槽负责，数据索引仍由 `BTreeRdma` 负责。`gap-lock`、`next-key-lock`、谓词区间和非叶层级协议继续使用 `RemoteLockManager` 的范围表回退，不能将精确对象路径的结果与这些协议混组。

## 服务几何

锁服务需要用 `rrl_rdma_lock_server <port> --objects <config>` 启动。`scripts/shared_rdma_services.py` 在配置含 `native_object_locks: 1` 时自动选择对象服务；对象目录指纹仍由 workload trace 校验，连接失败或几何不匹配直接停止。

## 约束

这是显式迁移开关，不改变默认正式矩阵。先通过构建、CTest 和定向 object-lock 回归，再单独建立 native-object 的 smoke 基线；正式矩阵不得把该路径与旧 `rdma-record-2pl` 数据混在一起。

## 后续

下一阶段再为 Gap/Next-Key 设计有序远端区间索引；在此之前不删除 `RemoteLockManager` 的 gate 回退，也不修改 `RemoteLockRegion` 的注册布局。
