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

`run_hierarchical_lock()` 使用逻辑根节点和可配置 key bucket 表示多粒度锁。范围读采用 Root IS + bucket S，写操作采用 Root IX + bucket X；`--bucket` 控制桶宽。`test/v6_hierarchical_lock_test.cpp` 验证意向锁兼容、同桶冲突和模式升级。

## RDMA 接入

`RemoteLockManager` 实现同一 `IRangeLockManager` 接口，远程锁表放在 `RemoteLockRegion` 注册内存中。`rrl_rdma_lock_server` 提供 RC QP 服务，`rrl_workload_runner --cc rdma-next-key` 和 `--cc rdma-hierarchical` 分别接入 v5/v6。当前版本使用一个 RDMA CAS gate 串行化槽表扫描与更新，作为正确性基线；后续再按 bucket 分片优化。
