# RDMA native policy 批量接入（2026-11-06）

`run_policy_workload` 是正式矩阵入口。此前 `run_next_key_lock` 的批量能力只覆盖旧版非 measured 入口，正式矩阵仍在事务状态客户端内逐个调用物理锁。现在 `NativePolicyClient` 和 `LocalNativePolicyClient` 都增加了 `acquire_batch`：

- 先检查同一事务状态槽；
- 对支持 `IRangeLockBatch` 的物理后端，一次完成多个区间的冲突判断与发布；
- wait-die / wound-wait 使用同一批次返回的冲突事务集合，按原年龄规则等待或标记 victim；
- 批量失败不发布部分区间，事务清理仍由原有 `unlock_all` 完成；
- 不支持批量能力的 TCP/旧锁管理器逐锁回退。

因此正式 RDMA 路径的协议分工是：

| 协议 | 远端物理授权 | 批量范围快路径 |
|---|---|---|
| record-2PL | 对象目录 CAS（显式 native-object 配置） | 单对象授权 |
| gap-lock | RemoteLockManager 区间槽 | 一次范围读的三个 gap 区间批量发布 |
| next-key-lock | RemoteLockManager 区间槽 | 一次范围读的全部 next-key 区间批量发布 |
| predicate | RemoteLockManager 区间槽 | 单谓词区间保持原语义 |
| hierarchy | 叶对象 CAS 或 bucket 区间槽 | 按层级顺序执行，暂不跨模式合并 |

本阶段不改变公共 `IRangeLockManager` 签名，也不把 predicate/next-key 误化成记录对象锁。构建和 CTest 45/45 通过；未启动正式矩阵。
