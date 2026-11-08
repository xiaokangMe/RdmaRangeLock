# 4. 从记录锁到 Next-Key Lock

> 复现口径审计（2026-11-07，代码基线 9db05f7）：当前机制、论文对应与差异以 [复现审计](../notes/paper-reproduction-audit-2026-11-07.md) 为准。下方旧矩阵保留其 commit 与数值；不能据此宣称复现了 InnoDB/ARIES/KVL 或证明新代码性能。

## 本节要回答的问题

- [ ] Record-Level Strict 2PL：strict/rigorous 持锁如何避免读取未提交写入与级联回滚
- [ ] 只有记录锁时幻读如何发生——给出可复现的交错序列
- [ ] Gap Lock 如何拦截插入：锁的不是行，是"位置"
- [ ] Next-Key Lock 为什么把记录锁与间隙锁合并成一个区间 `(prev, curr]`
- [x] 三种方案在锁数量、阻塞率、并发度上的定量差异（见文末“实测”）

## 论文机制与当前实现的关系

记录级两阶段锁保护实际访问的记录；范围内原本不存在的键没有被扫描记录锁覆盖，因而允许幻读。谓词锁还保护不存在的匹配记录。Eswaran 的条件 (14) 在一维区间下就是“区间相交且至少一个写者” [@eswaran1976]；record 与 predicate 的隔离保证不同，比较性能时必须同时说明。

经典 next-key 的 (prev,curr] 边界来自相邻现存索引键，而不是任意查询端点；最右侧需要真正的 +∞ 哨兵 [@graefe2010]（§5.1）。ARIES/KVL 还包含 IX/SIX、instant 检查与锁状态传播，不能用 Berenson 的隔离定义代替这些实现 [@mohan1990kvl]。

当前代码的实际状态如下：

| 入口 | 扫描加锁方式 | 复现定位 |
|---|---|---|
| record-2pl | 返回记录 S，锁后复扫 | 记录锁基线；不防范围幻读 |
| gap-lock | 相邻物理键开间隙 guard + Record S/X + InsertIntent | InnoDB 纯抑制规则的统一锁表适配；包含 ghost 记录保护 |
| next-key | 真实相邻键 (prev,k]，首尾用显式无穷；删除保护相邻两单元 | Graefe 基础 S/X 机制的稳定数值区间适配；非完整 ARIES/KVL |
| prior-key | 真实相邻键 [k,next)，首尾用显式无穷；删除保护相邻两单元 | next-key 镜像基础机制，与 next-key 共用规划及验证路径 |
| predicate / exact-range | 每个逻辑范围一把 S [lo,hi)，点写 X | 同一种一维谓词锁机制，两个兼容入口 |

2026-11-07 修正前的 gap/next-key 扫描保护并集均为 [lo,hi]，旧实验主要比较查询区间的表示和执行开销。修正后使用实际邻接键和显式无穷，gap 引入纯抑制兼容与插入意向 [@innodb84locking]；新旧代码的性能数据不能混组。实现差异和正确性验证见 docs/notes/classic-key-locks-implementation-2026-11-07.md。

predicate 与 exact-range 仅 kind/cache 身份不同；该标签不改变冲突规则，也不隔离跨事务冲突。正文统计为一个机制，保留旧命令和旧数据标签以便追溯。它是有明确论文依据的受限复现，不是 Griffin precision locking，也不能自动称为工业系统做不到的性能上界。

## 可复现的幻读交错（v3）

| 步骤 | T1 | T2 |
|---|---|---|
| 1 | BEGIN; SELECT * WHERE k BETWEEN 20 AND 30 → 3 行 | |
| 2 | | INSERT k=25; COMMIT |
| 3 | SELECT * WHERE k BETWEEN 20 AND 30 → **4 行** | |

T1 的两次读不一致。记录锁无法阻止，因为步骤 2 插入的行**在步骤 1 时还不存在**，没有可锁的对象。

## 与实现的对应

- 公用事务语义：src/txn/policy_workload.cpp；兼容驱动：src/txn/workload.cpp
- 公用锁表：src/lock/；区间与兼容：include/common/range.h、types.h
- 幻读反例与防护：test/v3_phantom_test.cpp、v4_gap_lock_test.cpp、v5_next_key_test.cpp；v3 测试通过表示成功展示幻读，不是允许测试失败
- 论文语义契约：test/locking_paper_contract.h，本地及真实 RDMA 后端共用
- 旧数据：experiments/results/，每个表按其代码 commit 解释

## 实测：三者的锁数量、吞吐与中止率

数据来自本地正式矩阵（756 次运行，10 秒窗口，三次重复取中位数），
campaign `verified-20261102-225419`，commit `8511d5b`，原始文件
`formal-local-20261102-225727.csv`。报告见 `notes/formal-verified-2026-11-03.md`，
表在 `figures/formal-verified-2026-11-03/`。

### 锁数量：v4 比 v3 多，v5 与 v4 相当

YCSB-E，单线程，按"每返回一行付出多少把锁"折算（去除扫描宽度的影响）：

| range | v3 record-2PL | v4 gap lock | v5 next-key |
|---:|---:|---:|---:|
| 8 | 1.007 | 1.382 | 1.257 |
| 32 | 1.002 | 1.095 | 1.064 |
| 128 | 1.000 | 1.024 | 1.016 |

在该批实现与负载下，三条都趋近"一行一把锁"；这不是所有范围锁的语义下界，完整谓词可由一把区间锁保护。v4 在窄范围上多出的 38% 是
区间两端的额外间隙锁，range 变大后被摊薄。

### 吞吐：单线程下 v3 与 v5 相近，v4 最慢

YCSB-E，t=1，committed txn/s：

| range | v3 | v4 | v5 | v6（b256，对照） |
|---:|---:|---:|---:|---:|
| 8 | 32 403 | 26 195 | 40 666 | 138 971 |
| 32 | 8 945 | 8 192 | 12 732 | 76 550 |
| 128 | 2 212 | 2 079 | 3 003 | 27 459 |

**v5 比 v3 快**这一点与"锁更多应该更慢"的直觉相反，此处只记录事实，不下结论。
两者的锁条目数几乎相同（1 234.8 vs 1 215.6），差异必须来自锁表内部的比较与存储方式，
候选解释有两个，都还没有验证：

1. v5 的条目是 `(prev, curr]` 区间，v3 是 `closed(k, k)` 点锁；区间比较在
   `RangeLock::overlaps` 里走的判定分支不同，可能更早短路；
2. 两种实现的 `lock_acquires` 分布不同，导致 `LockManager` 分片内的条目顺序不同，
   而查找是线性扫描。

要判定是哪一条，需要给锁表的比较次数加计数器后重跑同一组 case——这是一条待验证猜想，
不是本节的结论。

### 中止率：写冲突负载上三者几乎无差

以下只报告该批 no-wait 冲突中止率；没有实际等待，不能等同阻塞率。t=32：

| workload | v3 | v4 | v5 |
|---|---:|---:|---:|
| YCSB-A（50% 更新） | 90.8% | 90.3% | 90.8% |
| YCSB-E（范围读 + 插入） | 29.0% | 29.7% | 29.9% |

**v4/v5 多出来的锁没有换来更高的中止率**，因为它们多锁的是"间隙"，而这份负载的写
几乎都落在已有 key 上。这也说明：在"幻读能否被阻止"这个问题上，三者的差别是正确性，
不是性能——v3 在同样的中止率下仍然会放出幻读。

## 待补充

- 插入意向锁（insert intention lock）如何与 gap lock 交互
- 唯一索引与非唯一索引下 gap 的边界差异
- ~~死锁发生率：v4 与 v5 在同一 workload 下的对比~~ → 已用 no-wait 中止率替代（上表），
  本次已增加 wait-die / wound-wait 的本地及服务端协同 RDMA 路径；两者属于死锁预防，不是死锁检测。尚无对应正式矩阵，不能把上表扩展解释为等待策略比较


## 实现补充：冲突策略与谓词范围（2026-11-03）

记录/间隙/next-key/层级/谓词决定“锁什么”，no-wait / wait-die / wound-wait 决定“冲突时如何处理”，是两个独立维度。新统一事务驱动在三种策略间保留协议的锁对象差异，重试继承逻辑年龄，并区分成功 attempt 延迟、含重试完成延迟与未完成比例。谓词模式先锁完整半开区间再扫描，保护空结果区间；范围仅限一维 key。

当前 native 本地与 RDMA 共享事务语义，RDMA 使用单边状态槽；旧 TCP 兼容协调路径另行标注，不是 native 每事务必经层。比较应固定后端与控制面，原历史 NO_WAIT 图不作为新路径基线。确定性用例和小型双机检查仅验证实现，不形成新的吞吐或尾延迟结论。代码与证据入口见 [实现记录](../notes/wait-policies-and-latency-2026-11-03.md)。

## Precision Locks 与 Griffin：复现补充（2026-11-08）

本补充回答以下问题：

- [x] PL 与区间谓词锁的检查对象有什么差异？
- [x] 为何不检查 Lu/Lu 仍须保护写写冲突？
- [x] Griffin 如何避免异步有序索引返回旧结果？
- [x] 哪些是论文机制，哪些是统一架构适配？

Jordan 的 Precision Locks 把读谓词放入 Lp，把实际更新元组放入 Lu，双向检查 P(t)，避免谓词与谓词的可满足性判断；原文允许任意可独立求值的谓词 [@jordan1981precision]。写写互斥依赖写操作隐含的读谓词，包括唯一键插入前的不存在检查，不能仅实现两张列表便省略这一步。本系统实现 uint64 单键区间子集，Lu 只允许实际更新点；所有修改、删除与插入均先登记点 Lp。

Griffin 继承 PL 的成员集合防幻读机制，将点查放到 hash、范围查放到仅存 key 的 B+ 树，专用线程异步传播已提交插入/删除。已提交 Lu 在树同步完成前不能回收，否则扫描可漏掉已提交的新 key [@nakazono2024griffin]。本系统以无事务身份的 pending 复制元数据保留这项保护，并在扫描时独立统计同步滞后中止。

原 Griffin 的实验没有实现记录内容隔离，YCSB-A 对索引而言全部为 Lookup。本系统保留标准 A 的实际更新和多操作事务，因此在 Griffin membership PL 外补上 record strict 2PL，并采用统一私有写集延迟写入代替 abort 物理回滚。原文 lock-free hash / BwTree / 链表换成项目的分片 hash、共用 BTreeLatch 与分片锁表；这是机制复现，不是原系统性能复刻。纯锁机制比较固定同一索引后端，混合索引单独比较。

实现、原文页码、源码搜索结果、正确性用例与 RDMA 边界见 [复现验收记录](../notes/precision-griffin-reproduction-2026-11-08.md)。RDMA 当前复现 PL 部件，未实现远端 hash 混合 Griffin；Griffin-PPL 未获得全文，未实现。不将任何旧矩阵结果归到这两种新协议。
