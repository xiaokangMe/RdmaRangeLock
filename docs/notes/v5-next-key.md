# v5 Next-Key 阅读与实现定位

> 2026-11-07 校正：当前入口是分段查询区间保护的项目实现，不是完整经典 next-key / ARIES/KVL 复现。详见 [复现审计](paper-reproduction-audit-2026-11-07.md)。

## 论文依据与差异

Graefe2010 §5.1 定义边界来自相邻现存索引键，末端使用 +∞。C. Mohan 的 ARIES/KVL 发表在 VLDB 1990 pp.392–405，包含 IX/SIX、instant 及 lock state replication；并非旧笔记所写的 VLDB 1992，全文现已纳入文献核查。Berenson1995 支撑 phantom/长谓词锁的语义要求；Comer1979 支撑树结构背景，都不能代替具体锁算法。

当前 run_policy_workload 及 run_next_key_lock 对 [lo,hi) 先扫描，持 point(lo)、按记录分段的 (prev,k]、尾部 (last,hi]，锁后复扫验证键与读取值。点操作使用 S/X 点锁，写缓冲提交后统一释放。扫描保护并集为 [lo,hi]，保守多锁 hi。

lo/hi 不必是索引键，hi 也不是 supremum。显式区间按数值保持覆盖，因此当前防护来自谓词锁的区间覆盖不变式，而非完整的索引键锁名、间隙继承与 KVL 模式。不可据现有吞吐证明原论文 next-key 性能。

## 迁移要求

按审计 P0：统一索引邻居快照能力 → 选定基础 S/X next-key → 完成各操作与无穷边界 → 锁后重定位/SMO 覆盖 → 本地/RDMA 相同交错验证。保持统一索引和事务骨架，不为协议另造一棵树，不改变公共锁管理器签名。

## 当前验证与策略

test/v5_next_key_test.cpp 验证显式半开区间防护；policy_scan/own_write/native 等检查读取及释放；这些不代表缺失的实际邻接键已实现。统一驱动支持 no-wait/wait-die/wound-wait；等待策略与锁对象是不同维度。
