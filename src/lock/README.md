# src/lock

事务级锁的实现。四个版本按递进关系排布，每个版本只在前一个版本上增加**一种**机制，便于做单变量对比。

| 版本 | 文件（待实现） | 新增机制 | 相对上一版的差异点 |
|---|---|---|---|
| v3 | `v3_record_2pl.cpp` | 记录级 strict 2PL | 基线：只能锁已存在的行 |
| v4 | `v4_gap_lock.cpp` | 间隙锁 | 锁对象从"行"扩展到"行之间的位置" |
| v5 | `v5_next_key_lock.cpp` | next-key 区间锁 | 把 record + gap 合并为 `(prev, curr]` |
| v6 | `v6_next_key_hierarchical.cpp` | 多粒度层级 + 意向锁 | 在祖先节点加 IS/IX，避免逐个锁叶子 |

## 共用接口

全部版本实现 `include/lock/lock_manager.h` 中的同一组接口：

```
lock(mode, key_range)      // 加锁，mode ∈ {S, X}
unlock_all()               // 事务结束统一释放（strict 2PL）
get_lock_count()           // 供 statistics/ 采集
detect_conflict(other)     // 区间相交判定，各版本实现不同
```

**接口一致是硬约束**：只有这样 v3~v6 才能跑同一份 workload、同一份测试用例，得到的数字才有可比性。

## 递进关系与验证方式

每加一层机制，必须同时给出两件事：

1. **正确性提升的证据**：`test/phantom_*.cpp` 里失败用例变成通过
2. **代价的量化**：`experiments/results/` 里锁数量、阻塞率、吞吐的变化

只做 1 不做 2，等于没有说明"这个机制值不值"。

## 实现顺序建议

v3 → v5 → v4 → v6。

先跳过 gap lock 直接做 next-key，是因为 v5 的结构更简单（一个区间锁 vs 两种锁的组合）。等 v5 正确后，再回头做 v4 作为"拆分版"，用来对比"合并 vs 分离"两种表达方式的差异——这本身就是一个值得写进文章的观察。

## 注意事项

- strict 2PL 的释放时机：**只能在 commit/abort 后统一释放**，任何提前释放都会破坏可串行化
- 锁表的哈希桶与死锁检测彼此独立，不要耦合在一起
- v6 的加锁顺序必须自顶向下，违反顺序必然死锁
