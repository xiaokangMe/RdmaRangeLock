# v5 Next-Key Lock 阅读与实现笔记

## 论文依据

本项目先精读了 Berenson 等人的 *A Critique of ANSI SQL Isolation Levels*（SIGMOD 1995，PDF：`papers/berenson-1995-isolation-levels.pdf`）。论文把 phantom 定义为同一谓词两次执行得到不同的记录集合，并指出只锁住第一次读到的记录不足以保证谓词稳定性；并发插入必须与谓词对应的 key range 冲突。

Comer 的 *The Ubiquitous B-Tree*（ACM Computing Surveys 1979，PDF：`papers/comer-1979-ubiquitous-b-tree.pdf`）用于确认范围扫描的有序叶链和前驱/后继关系。实现中的 next-key 区间直接建立在 B+树扫描返回的有序 key 序列上。

ARIES/KVL 的书目信息为 C. Mohan et al., “ARIES/KVL: A Key-Value Locking Method for Concurrency Control of Multiaction Transactions Operating on B-Tree Indexes”, VLDB 1992。当前 VLDB 镜像对该 PDF 返回 403/404，因此本仓库不伪造下载文件；其 key-value locking 语义由 Berenson 的谓词保护结论和本地测试共同约束。

## 区间分解

对半开扫描 `[lo, hi)`，扫描得到的有序 key `k_i` 被分解为 `(prev, k_i]`，其中 `prev` 初始为 `lo`；最后增加 `(last, hi]` 的 supremum 区间。这样插入一个落在扫描间隙中的 key 会与已有的 S next-key lock 相交。点读、更新、删除和插入使用闭区间点锁；写集合在提交时统一应用，锁在提交或回滚后释放，保持 strict 2PL。

实现位置：`src/txn/workload.cpp::run_next_key_lock`；锁对象和边界相交判断仍由 `LockManager` 与 `KeyRangeEnt` 统一处理。测试覆盖两个相邻 next-key 区间、间隙插入、范围外插入和空结果范围。

## 限制

当前锁表采用 no-wait 冲突返回 `Abort`，没有等待图或死锁检测；这是为了让 v3-v6 在同一调度器下只比较锁粒度和冲突语义。前驱只取 workload 扫描下界，而不是从索引叶页提取全局前驱，因此实验接口不会暴露页结构，仍能验证范围内 phantom 防护。