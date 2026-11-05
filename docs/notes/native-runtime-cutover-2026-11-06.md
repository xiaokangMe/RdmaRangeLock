# Deneva 式 native runtime 切换方案（2026-11-06）

## 目标

默认事务路径采用 Deneva 对应的状态槽/CAS/物理锁直接授权路径：

```
索引读取 -> 物理锁表冲突读取 -> 事务状态 CAS -> 本地年龄判断
-> 数据写回 -> 物理锁释放 -> 状态槽提交/结束
```

本地路径使用进程内原子状态槽和已有分片 `IRangeLockManager`；跨主机路径使用注册内存事务状态槽、RDMA CAS 和 `RemoteLockManager`。旧 `TransactionCoordinator` 与 TCP `PolicyClient` 只作为显式兼容/正确性基线，不再作为默认性能路径。

## 保留边界

- 保留 `IRangeLockManager`、record-2PL、gap-lock、next-key-lock 和层级锁的范围语义。
- 不把 Deneva 的记录锁算法误称为本项目的范围锁实现。
- 不复制 Deneva 源码中已发现的无效比较和越界写回问题。
- 原生路径不增加逐事务/逐锁 TCP 授权；TCP 仅保留建链、控制和显式兼容后端用途。

## 验收顺序

1. 本地 native 状态槽与物理锁直接授权；
2. 本地 wait-die/wound-wait、代次复用和提交/中止竞态测试；
3. 跨主机原生状态槽与物理锁路径回归；
4. CTest 全部通过；
5. 定向短负载确认默认后端和指标；
6. 重新建立正式矩阵基线。正式矩阵在此方案验收前不启动。
