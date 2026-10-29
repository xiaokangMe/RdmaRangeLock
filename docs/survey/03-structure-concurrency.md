# 3. 结构并发：Latch Coupling

## 本节要回答的问题

- [ ] latch 与 lock 的区别：为什么两者必须分层，混用会怎样
- [ ] latch coupling（crabbing）的加锁顺序与释放时机
- [ ] 为什么"自顶向下、父先于子"能避免死锁
- [ ] 读优化（latch crabbing vs. optimistic descent / B-link 树）的取舍

## 核心机制草稿

latch 保护的是**内存结构的短期一致性**，生命周期是"一次操作"；lock 保护的是**逻辑数据的长期一致性**，生命周期是"一个事务"。两者混淆会产生两类错误：

- 用 latch 做事务隔离 → 事务未提交就释放，语义丢失
- 用 lock 做结构保护 → 锁开销与事务粒度绑定，结构操作无法高频进行

latch coupling 的做法：持有父节点 latch → 获取子节点 latch → 判定子节点是否"安全"（不会因本次操作而分裂/合并）→ 安全则释放父 latch。**安全判定是释放父锁的前提**，这一点是很多实现出 bug 的地方。

## 与实现的对应

- 实现：`src/index/`（v2），与 v1 共用同一套页布局
- 对比基线：v1 单线程
- 压力测试：`test/stress_latch_coupling.cpp`，检测结构损坏与死锁

## 评价指标

| 指标 | 采集位置 |
|---|---|
| latch 争用次数 / 等待时间 | `statistics/` |
| 吞吐随核数扩展曲线 | `experiments/results/` |
| 死锁检测触发次数（应为 0） | 运行日志 |

## 待补充

- B-link 树（带右指针的高并发 B+树）作为对照
- 乐观下降（optimistic descent）在读多写少负载下的收益
