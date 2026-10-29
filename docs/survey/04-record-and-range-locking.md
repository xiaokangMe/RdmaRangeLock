# 4. 从记录锁到 Next-Key Lock

## 本节要回答的问题

- [ ] Record-Level Strict 2PL：为什么 strict 是必须的（级联回滚）
- [ ] 只有记录锁时幻读如何发生——给出可复现的交错序列
- [ ] Gap Lock 如何拦截插入：锁的不是行，是"位置"
- [ ] Next-Key Lock 为什么把记录锁与间隙锁合并成一个区间 `(prev, curr]`
- [ ] 三种方案在锁数量、阻塞率、并发度上的定量差异

## 三个实现的关系

```
v3  Record-Level Strict 2PL
     锁集合 = { 匹配到的记录 }
     幻读：无法阻止          ← 反例

v4  Gap Lock
     锁集合 = { 记录 } ∪ { 记录之间的间隙 }
     幻读：阻止              锁数量：约为 v3 的两倍

v5  Next-Key Lock
     锁集合 = { (prev, curr] 区间锁 }
     幻读：阻止              锁数量：与 v4 相当，但语义更简洁
```

v4 → v5 的关键差别不在性能，而在**锁的语义表达力**：gap 与 record 分开时，需要额外规则描述"插入落到哪个间隙"；next-key 直接把"位置"和"值"统一进一个区间，冲突判定变成区间相交测试。

## 可复现的幻读交错（v3）

| 步骤 | T1 | T2 |
|---|---|---|
| 1 | BEGIN; SELECT * WHERE k BETWEEN 20 AND 30 → 3 行 | |
| 2 | | INSERT k=25; COMMIT |
| 3 | SELECT * WHERE k BETWEEN 20 AND 30 → **4 行** | |

T1 的两次读不一致。记录锁无法阻止，因为步骤 2 插入的行**在步骤 1 时还不存在**，没有可锁的对象。

## 与实现的对应

- `src/lock/`：v3 / v4 / v5
- 幻读用例：`test/phantom_*.cpp`，三个版本跑同一组用例，预期 v3 失败、v4/v5 通过
- 数据：`experiments/results/`

## 待补充

- 插入意向锁（insert intention lock）如何与 gap lock 交互
- 唯一索引与非唯一索引下 gap 的边界差异
- 死锁发生率：v4 与 v5 在同一 workload 下的对比
