# v3 记录级 Strict 2PL

v3 在统一 workload trace 上使用 `LockManager` 的 `LockKind::Record`。点读取得 S record lock，更新/插入/删除取得 X record lock；锁一直保留到事务 commit 或 abort，写集合在所有锁成功后统一应用。

范围读先枚举当前 B+树返回的记录，再逐条取得 S record lock。它不会锁住空隙，因此另一个事务可以对结果集中原本不存在的 key 取得 X record lock 并插入。`test/v3_phantom_test.cpp` 固定复现了这一点：T1 锁住 10、20、30，T2 插入 15，T1 第二次扫描得到 4 行。

当前锁管理器采用 no-wait 冲突返回 `Abort`，不在锁持有期间做 I/O 或等待。事务 runner 在 abort 时释放全部记录锁；由于写集合延迟到 commit 才应用，不会留下半个事务的更新。

## 运行

```text
rrl_workload_runner --index v2 --cc record-2pl \
  --workload ycsb-a --keys 10000 --txns 5000 \
  --ops 10 --threads 4 --zipf 0.99
```

YCSB-A 的高冲突结果可以直接观察 `aborted` 和 `lock_conflicts`。YCSB-E 范围读只锁已有记录，幻读语义由专门测试验证；大规模范围读的锁数量随扫描行数增长，这是 v5/v6 的对照基线。