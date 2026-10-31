# BTreeLatch 根/写锁扩展性（2026-11-01）

## 修改

`BTreeLatch::insert` 增加 existing-key 快路径：先用 root shared latch 做 latch coupling，到叶子后只获取叶页的 exclusive latch 更新 value。已有 key 的更新不再经过 `writer_guard_`，也不再把 root page 以 exclusive 方式锁住。

新 key 插入仍使用原有 writer/root split 路径；这样保留根分裂、叶链和 `size_` 的正确性，同时把 TPC-C payment、YCSB-A 中占主要比例的更新从全局写锁临界区移出。

## 验证

Release 构建和 9 个 CTest 用例全部通过；32 线程、3 秒、每线程 2048 key 的压力测试通过：

```text
v2 B+ tree stress: OK threads=32 seconds=3 keys=65536 operations=6909983 height=7
```

同一份 runner 配置下，当前版本的无事务锁基线为：

| workload | t1 committed txn/s | t32 committed txn/s | t32/t1 |
|---|---:|---:|---:|
| YCSB-A | 270,507 | 57,679 | 0.21 |
| TPC-C payment | 621,341 | 391,050 | 0.63 |

TPC-C payment 全部是 existing-key update，因此 t32/t1 从此前报告的约 0.32 提升到约 0.63。YCSB-A 仍包含读和更新，扩展性也有所改善，但 root page 在新 key 插入和结构变化时仍会进入写路径。

## 仍待观察

新 key 高并发插入和删除仍共享 writer/root split 路径；后续正式矩阵应把“existing-key update”和“new-key insert”拆成两组，避免把两种临界区混成一条曲线。
