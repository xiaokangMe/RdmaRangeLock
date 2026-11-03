# src/lock

事务级锁的实现。四个版本按递进关系排布，每个版本只在前一个版本上增加一种机制，便于做单变量对比。

| 版本 | 文件（待实现） | 新增机制 | 相对上一版的差异点 |
|---|---|---|---|
| v3 | `v3_record_2pl.cpp` | 记录级 strict 2PL | 基线：只能锁已存在的行 |
| v4 | `v4_gap_lock.cpp` | 间隙锁 | 锁对象从“行”扩展到“行之间的位置” |
| v5 | `v5_next_key_lock.cpp` | next-key 区间锁 | 把 record + gap 合并为 `(prev, curr]` |
| v6 | `v6_next_key_hierarchical.cpp` | 多粒度层级 + 意向锁 | 在祖先节点加 IS/IX，避免逐个锁叶子 |

## M1 公共接口

M1 已提供 `include/lock/lock_manager.h` 和 `include/common/range.h`：

```cpp
RC lock(txnid_t txn, LockMode mode, const KeyRangeEnt& range,
        LockKind kind = LockKind::Predicate);
RC unlock(txnid_t txn, const KeyRangeEnt& range);
RC unlock_all(txnid_t txn);
std::size_t get_lock_count(txnid_t txn = 0) const;
```

`KeyXidCache` 保留 deneva 的 `addReadXidWithMutex`、`addWriteXidWithMutex`、`getReadXid` 和 `getWriteXid` 命名，内部转发到 `LockManager`。公开接口只表达事务、模式和区间；记录、间隙、next-key、谓词和节点语义通过 `LockKind` 区分。

## 递进关系与验证方式

每加一层机制，必须同时给出两件事：

1. `test/phantom_*.cpp` 中的正确性证据；
2. `experiments/results/` 中锁数量、阻塞率和吞吐变化。

## 实现顺序建议

v3 → v5 → v4 → v6。

先跳过 gap lock 直接做 next-key，是因为 v5 可以先用一个区间锁表达。等 v5 正确后，再回头做 v4 的记录锁与间隙锁拆分，用于比较两种表达方式。

## 注意事项

- strict 2PL 的释放时机：只能在 commit/abort 后统一释放。
- 锁表的互斥保护与死锁检测彼此独立。
- v6 的加锁顺序必须自顶向下；读取子树应使用祖先 IS 加目标节点 S，修改子树应使用祖先 IX 加目标节点 X。
## v3 当前实现

un_record_2pl() 位于 src/txn/workload.cpp，锁对象和冲突判定仍由 LockManager 统一提供。该版本只传入 KeyRangeEnt::point(key) 与 LockKind::Record，因此没有 gap/predicate 保护；写集合延迟到提交阶段应用，严格释放发生在 commit/abort 之后。

## v4 当前实现

un_gap_lock() 在 v3 的记录锁外增加开区间 Gap S 锁和边界 Gap S 锁；插入使用目标 key 的 Gap X 锁。对应的范围交集和冲突仍由 LockManager 完成，正确性证据在 	est/v4_gap_lock_test.cpp。

## v5 当前实现

`run_next_key_lock()` 位于 `src/txn/workload.cpp`。范围扫描按 `(prev, curr]` 分解，并补充 `(last, supremum]` 尾区间；插入、更新和删除使用点 next-key X 锁。`test/v5_next_key_test.cpp` 验证间隙插入冲突和范围外插入放行。

## v6 当前实现

`run_hierarchical_lock()` 使用根、逻辑 key bucket、记录三层资源。点读采用 Root IS + bucket IS + record S，点写采用 Root IX + bucket IX + record X；范围读锁住覆盖桶 S。同桶同时含扫描和写时，预先选桶 X（当前没有 SIX），不再取得被桶 S/X 覆盖的记录锁。访问集来自已生成 trace，在测量窗口前排序、去重并确定最终模式；按根、升序桶、升序记录申请，执行阶段不升级。所有锁仍在提交/中止决定后统一释放，冲突策略保持 NO_WAIT。详见 `docs/notes/rdma-hierarchy-correctness-2026-11-03.md`。

`test/hierarchy_plan_test.cpp`、`test/rdma_hierarchy_test.cpp` 验证同桶不同记录并发、同记录互斥、空结果范围的插入保护、边界与失败清理。`--hierarchy-diagnostics 1` 或配置 `hierarchy_diagnostics: 1` 开启每 worker 的 JSONL 诊断，写入 stderr；默认关闭且正式 CSV 不变。阶段 0/1/2 分别是根/桶/记录，mode 0/1/2/3 是 IS/IX/S/X。统计最长无提交时间、最长连续中止数、最后冲突请求及单次申请/释放的最大耗时，统一在运行结束后输出。

远端事务操作条带、共享 QP 和本地 gate 使用条件变量 FIFO 准入。该队列只调度本地执行资源，不把事务锁变成等待锁。NO_WAIT 下冲突事务仍可能中止；不能将 FIFO 解释为所有热点事务都获得无饥饿保证。

## RDMA 接入

`RemoteLockManager` 实现同一 `IRangeLockManager` 接口，远程锁表放在 `RemoteLockRegion` 注册内存中。`rrl_rdma_lock_server` 提供 RC QP 服务，`rrl_workload_runner --cc rdma-next-key` 和 `--cc rdma-hierarchical` 分别接入 v5/v6。当前版本使用一个 RDMA CAS gate 串行化槽表扫描与更新，作为正确性基线；后续再按 bucket 分片优化。
