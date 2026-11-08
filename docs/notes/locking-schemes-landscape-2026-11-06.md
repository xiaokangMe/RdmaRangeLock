# 范围锁综述：尚未覆盖的加锁方案调研

> **性质**：阅读笔记 / 选题建议，**不是综述正文**。按 `docs/README.md` 的约定，不确定的猜想放 `notes/`，验证之后再挪进 `survey/`。
> **调研日期**：2026-11-06
> **核实标记**：🟢 已核对原始出处（论文原文 / 官方技术文档）｜🟡 仅二手来源或出处待核
> **背景**：当前稿件 `docs/survey/00~06` + `.staging/RdmaRangeLock/docs/survey/`，实现为 v1–v6，实验含 no-wait / wait-die / wound-wait 三策略与本地 / 跨主机两套矩阵。

---

## 0. 结论先行

1. **最大的结构性缺口是「谓词锁」这一支**：`00-outline.md` 的分类表里把「谓词本身」列为一种锁载体、`06` 章也提到 SSI 和学习型索引，但正文从未展开 predicate lock / precision lock / PAX / InnoDB 空间索引谓词锁。这条支线恰好能回答第 6 章「索引依赖的代价」那个论点。
2. **v5 next-key 不是终点，而是「资源映射」三种选择中的一种**。Lomet 1993 的 KRL、ARIES/KVL、ARIES/IM 分别对应「range/key 分离」「range+key 合并」「range+key+record 全合并」，并且有比 S/X 丰富得多的锁模式（IIn/ID/IU 等）。我们的 v4/v5 只覆盖了其中两点。
3. **v6 的「层级桶锁」其实不是 Gray 式意向锁层级，而是固定桶宽的粗粒度范围锁**。文献里对应的正解是 Graefe 的「内部节点 separator key / key 前缀」层级 + 在线粒度调整（escalation / de-escalation），这直接回答组会 P21 留下的问题「桶宽是否只能当超参」。
4. **综述目前只讨论「锁语义」（锁什么），缺「锁服务实现」这一维**（锁管理器放在哪、一次加锁几次往返）。仓库命名即 RDMA，而 Sherman / DecLock / Lotus 正是针对「每把锁 5.6 个单边往返」这一类问题的另一条路线。
5. **有不少「待补充」其实是现成答案**：插入意向锁、唯一索引边界、B-link / 乐观下降、锁升级，都有官方文档或经典论文可直接引用，不必新做实验。

---

## 1. 现状与缺口对照

| 已有 | 覆盖到的维度 | 未覆盖的相邻方案 |
|---|---|---|
| v1 单线程 B+树 / v2 latch coupling | 结构层 latch | B-link 树、乐观下降 / OLC、Bw-tree、Foster B-tree、ghost records |
| v3 record-2PL | 只锁已存在的行 | 页锁 / 表锁（粗粒度基线）、锁升级 |
| v4 gap lock / v5 next-key | range+key 合并、`(prev, curr]` | KVL 的 instant IX 检查、KRL 的 range/key 分离、SQL Server RangeS-S/RangeI-N、插入意向锁、supremum 伪记录 |
| v6 层级桶锁 | 固定桶宽粗粒度 + IS/IX | separator key 层级、key 前缀层级、动态粒度调整、U/SIX/SIU/UIX 与转换锁 |
| 三等待策略 | 冲突后的行为 | 死锁检测（wait-for graph）、队列锁 / FIFO 公平性、MCS lock |
| 跨主机 RDMA 锁表 | 远程锁表的代价实测 | 片上锁表、本地锁委托与 handover、解耦锁、锁分片到计算池 |
| 第 6 章 SSI / MVCC | 只作冲突讨论 | SIREAD 谓词锁、SIREAD 粒度提升、OCC 节点集验证、MVCC+precision validation、timestamp cache |

另有一个**写作口径问题需要讨论**：`README.md` / `CONSTRAINTS.md` 规定「没有对应实现的论断，不写进文章」。但综述天然需要文献性论断。建议二选一：
- (a) 增设独立的「相关工作」小节，明确标注为文献性论断、不带性能数字（只讲机制与出处），与「实测章节」区分；
- (b) 只把能落地成 v7+ 的方案写进正文，其余留在 `notes/`。

---

## 2. 全景：四条轴上还有哪些加锁方案

### 轴 1 · 锁什么（锁对象 / 资源映射）

| 方案 | 锁对象 | 关键机制 | 出处与状态 |
|---|---|---|---|
| **Predicate lock** | 谓词本身（WHERE 子句） | S 锁谓词、X 锁写入谓词；冲突 = 两个谓词可同时满足。简单谓词的可满足性判定已是 NP 完全 | Eswaran, Gray, Lorie, Traiger, CACM 19(11), 1976 🟢 |
| **Precision lock (PL)** | 「恰好需要的元组集合」，含幻影位 | 用实际数据实例判断冲突而非谓词空间相交，宣称达到元组级方法中最优并发；谓词可任意复杂 | Jordan, Banerjee, Batman, SIGMOD 1981, 143–147 🟢 |
| **PAX / Predicate Assertion** | 读谓词「断言」+ 索引锁混合 | 在内存中断言读谓词并比对更新，作为 KVL/IM 的扩展以减少假冲突 | O'Neil, Fekete, O'Neil, Liarokapis, 2000 🟡（会议出处待核） |
| **空间索引谓词锁（生产实现）** | 查询 MBR（最小外接矩形） | R-tree 无全序、定义不了 "next key"，故锁挂在索引页 infimum 记录上、独立 lock hash、按 MBR 判冲突、节点分裂时向下传播 | MySQL WL#6609 / 5.7.5 release notes 🟢 |
| **ARIES/KVL** | key value = `(prev, curr]` 区间 | 未命中也锁 next key；插入时对 next key 做 **instant IX**（只检查不持有），删除时持 commit-duration X；用 IX/SIX 提高并发；唯一/非唯一索引规则不同 | Mohan, VLDB 1990；`Repeating History Beyond ARIES` §3.8.1 🟢 |
| **ARIES/IM** | 记录 rid（不锁索引 key） | 多二级索引只需一把锁，开销最低；但一把 record 锁会连带锁住不参与扫描的 key 范围，并发度下降；页分裂不持 commit-duration 锁 | Mohan & Levine, SIGMOD 1992；同上 §3.8.2 🟢 |
| **KRL（Lomet）** | 三种资源映射可选 | ① range 独立 ② range+key 合并 ③ range+key+record 合并；新复合模式 `IS/-S`、`IIn/-`(instant)、`ID/-`、`IU/-X`、`IIn/-X`、`S`、`SIX`、`X`；允许「插入互不打挡」「单例读旁可插入/删除」，ARIES/KVL 做不到 | Lomet, VLDB 1993, P655（原文 §4/§5 已核）🟢 |
| **SQL Server key-range lock** | 区间 + 行两部分 | `RangeS-S` / `RangeS-U` / `RangeI-N`（插入前测区间）/ `RangeX-X`，另有 RangeI-S/RangeX-U 等转换锁与完整兼容矩阵；要求 SERIALIZABLE + 索引覆盖谓词 | Microsoft Learn, Key-Range Locking 🟢 |
| **InnoDB 插入意向锁 / supremum** | gap 上的「插入意图」 | 同 gap 内不同位置插入互不阻塞，但与被持有的 gap 锁冲突；上界用 supremum 伪记录；gap 锁「纯抑制」、可共存 | MySQL 8.4/9.0 Reference §17.7.1、§17.7.3 🟢 |

### 轴 2 · 用什么模式 / 锁多久（冲突判定与持锁时长）

| 方案 | 要点 | 出处 |
|---|---|---|
| **U / SIX / SIU / UIX、转换锁** | 更新锁避免「两个 S 同时升级」死锁；SQL Server 的 `sp_lock` 列出完整模式族 | MS Learn `sys.sp_lock` 🟢 |
| **Instant lock** | 只做兼容性检查、不持有到提交；KVL 的插入检查、KRL 的 IIn/ID instant 模式 | Lomet 1993、Mohan 1990 🟢 |
| **Ghost records / delete stub** | 删除只打标记，不合并区间，从而放宽删除的排他要求；代价是存储与 GC | Lomet 1993 §5.3；Graefe 2010 survey 🟢 |
| **Early lock release / Controlled lock violation** | 提交日志落盘前释放锁（或允许他人「有控制地违反」锁），把持锁时长砍到几分之一；对分布式事务与长 commit 路径尤其有意义 | Kimura/Graefe/Kuno, ADMS 2012；Graefe et al., SIGMOD 2013, DOI 10.1145/2463676.2465325 🟢 |
| **Increment / escrow / 语义锁** | 对可交换操作（+=、库存预占）用专门模式提高并发；Graefe 指出「update 锁」在有更多基础模式时已不足以命名 | Korth, JACM 1983；Graefe 2010 survey §5.6 🟡 |

### 轴 3 · 锁多大（粒度与层级）

| 方案 | 要点 | 出处 |
|---|---|---|
| **Graefe 三种层级设计** | ① 索引/叶页/key-range 传统层级 ② **内部节点 separator key** 解锁不同大小的 key 范围，随树层扩展、适应倾斜分布 ③ **复合 key 的前缀**锁，更接近谓词锁；配套 lock escalation / **de-escalation** / 在线增删层级而不中断事务 | Graefe, *Hierarchical Locking in B-Tree Indexes*, Synthesis Lectures on Data Management；US20110208704A1 🟢 |
| **Graefe B-tree 综述（应作为第 5 章主引）** | 明确区分 latch（线程/结构）与 lock（事务/内容）；总结 key range locking、ghost records、increment locks、separator key 层级，并给出「最先进设计 = key 与相邻 gap 分别加锁 + 严格多粒度」 | Graefe, ACM TODS 35(3), Article 16, 2010 🟢 |
| **Lock escalation / de-escalation** | SQL Server 单语句单表引用 ≥5000 把锁触发升级（每 1250 把检查一次），可升到 HoBT/分区；DB2 `NUMLKTS`/`MAXLOCKS`/`LOCKMAX 0`；Oracle 明确不做升级以避免升级死锁 | MS 文档 / DB2 文档 / *Database Tuning* 🟢 |
| **固定桶宽粗粒度（我们 v6 的位置）** | 桶宽是超参、随负载最优点漂移（本项目实测 ycsb-a 最优 16、ycsb-e 最优 256，t=32 差 4 倍） | 本项目 `README.md`、11-01/11-06 日志 |

> **关键对照**：v6 是「离线固定粒度」，Graefe 是「在线可变粒度 + 结构感知（separator / prefix）」。两者都是用少量粗粒度锁换锁数量，但后者不需要预先选桶宽，也不会因 key 分布倾斜而失效。

### 轴 4 · 锁在哪里 / 怎么实现（锁服务，RDMA 相关）

| 方案 | 要点 | 出处 |
|---|---|---|
| **Sherman HOCL** | 锁表放 RNIC 片上 SRAM（256KB 可容纳 131072 把 16-bit 锁，用 masked CAS 省空间）；计算节点再放本地锁表排队，同机冲突不发远程 CAS；**lock handover** 在同机线程间直接移交所有权，省一次 RTT；两级版本号 + 命令合并 | SIGMOD 2022 / arXiv 2112.07320 / CACM 2025 🟢 |
| **DecLock** | MN 侧只做原子入队（FAA），把所有权转移交给 CN 间 two-sided 通知；解决 MN-NIC 被 CAS 重试打爆的问题，比 RDMA spinlock 高 43.37× | arXiv 2505.17641, 2025 🟢 |
| **Lotus** | 「锁分离」：把锁按 key range 分片到计算池 CN，由 CPU 处理，绕开 MN RNIC；应用感知分片 + 两级均衡 + lock-first 协议 + 免重建恢复 | arXiv 2512.16136, 2025 🟢 |
| **FaRM / DrTM / FaSST / NAM-DB** | FaRM 用 RDMA CAS + 版本做 OCC 事务；DrTM 借 RDMA 与 HTM 一致性；FaSST 主张双边不可靠 verb 扩展性更好 | Sherman §6 相关工作 🟢 |
| **Spanner lock table** | Paxos leader 维护 2PL 锁表，**map ranges of keys → lock states**；读写事务读时取 S 锁并用 keepalive 保持，提交时取 X 锁；wound-wait 防死锁 | Corbett et al., OSDI 2012 §4.2.1 🟢 |

> 我们实测的「每把锁 5 个单边操作（gate CAS / 读计数 / 读槽 / 写槽 / 释放门）」是**锁服务实现**的代价，不是 next-key 语义的代价。HOCL 的本地委托、DecLock 的队列通知、Lotus 的锁分片，都是在不改锁语义的前提下压缩这个 5× 的三条不同路线。

### 轴 5 · 不加锁但解决同一问题（应作为第 6 章的对照组）

| 方案 | 幻读/写偏斜怎么处理 | 出处 |
|---|---|---|
| **SSI + SIREAD** | 「锁」不阻塞任何人，只标记读过的对象；检测 `T1 →rw T2 →rw T3` 危险结构并中止；**SIREAD 必须活到所有并发事务结束**；tuple→page→relation 逐级提升以控内存，粗粒度带来假阳性 | Cahill/Röhm/Fekete, SIGMOD 2008；Ports & Grittner, VLDB 2012；PostgreSQL `README-SSI` / commit `dafaa3ef` 🟢 |
| **OCC + NodeSet 验证** | 提交时重新验证扫描过的索引节点版本（Silo 的 node verify）；TicToc 明确说幻读要靠「索引加锁或验证阶段重扫」，并把 order-preserving index 的数据驱动时间戳列为 future work | Tu et al., SOSP 2013；Yu et al., SIGMOD 2016 §3.6 🟢 |
| **MVCC + precision-locking 验证** | 提交时验证「近期已提交事务的（外延）写」是否与本事务的（内涵）读谓词空间相交；undo buffer 保留 before-image delta | Neumann/Mühlbauer/Kemper, SIGMOD 2015 🟢 |
| **Timestamp cache / read span** | 读操作把读时间戳写进 **interval cache**，key 就是 key range；扫描把整段范围写进去，从而拦住「扫描时还不存在的 key」被低时间戳写入 | CockroachDB 官方博客 / 事务层文档 🟢 |
| **Griffin / Griffin-PPL** | 哈希 + B+树混合索引，用 precision locking 防幻读而不需要额外 B+树遍历；PPL 把锁链表去中心化、并行释放，50% insert + 50% scan 下 64 线程 6.5× | arXiv 2407.13294；CANDARW 2024 🟢 |
| **确定性排序（Calvin 类）** | 预先定序，执行阶段不需要冲突检测；作为「完全不用锁」的极端对照 | 教科书级，🟡 本轮未核原文 |

### 结构层（第 3 章「待补充」的现成答案）

- **B-link 树**（Lehman & Yao, TODS 1981）：每节点 high key + right link，读操作完全不加 latch，分裂被拆成「挂右兄弟」与「父节点插 separator」两步 🟢
- **乐观下降 / optimistic lock coupling (OLC)**：先以读锁下降，叶子不安全才重启为悲观路径；写者通常只锁一个叶节点 🟢
- **Bw-tree / BzTree / Foster B-tree**：以 CAS / 版本 / 无锁结构替代 latch coupling；Graefe 2010 §7 把「HTM 能否替代 latch」列为开放问题 🟡

---

## 3. 增补优先级（按「能不能直接进正文」排序）

| 级别 | 方案 | 建议落点 | 要不要新实现 | 与现有接口的关系 |
|---|---|---|---|---|
| **P0** | Graefe 2010 综述 + Lomet 1993 KRL + ARIES/KVL/IM | 第 4 章新增「4.5 资源映射与锁模式的其他选择」 | 不需要（文献机制） | 无冲突 |
| **P0** | Predicate / precision lock + InnoDB 空间索引谓词锁 | 第 6 章「索引依赖的代价」改写为有反例的论证 | 不需要 | 无冲突 |
| **P0** | SQL Server key-range 四模式 + InnoDB 插入意向锁 / supremum / 唯一索引边界 | 直接填掉第 4 章「待补充」两条 | 不需要 | 无冲突 |
| **P0** | SSI / SIREAD + 粒度提升 | 第 6 章「可串行化 vs 快照隔离」由口号变机制 | 不需要 | 无冲突 |
| **P1** | **v7：KRL 复合模式**（key 与 gap 分离 + IIn/ID instant） | 新一节，与 v4/v5 单变量对比「假冲突率」 | 需要 | ⚠️ 需把 `mode` 枚举从 {S,X} 扩展；按 `CONSTRAINTS.md` §2，接口变更要单独一次提交 |
| **P1** | **v8：separator key / key 前缀层级**（替代固定桶宽） | 第 5 章 + 回答组会 P21 | 需要 | 接口可不变（仍是 `lock(mode, key_range)`） |
| **P1** | **远程锁表本地委托 / handover / 批量解锁** | 第 6 章「远程锁表实测」新增一组消融 | 需要 | 只改锁服务实现，不动锁语义，最符合单变量约束 |
| **P2** | 锁升级 / de-escalation、early lock release、increment 锁 | 遗留问题清单 | 可暂不实现 | — |
| **P2** | B-link / OLC / Bw-tree、分布式范围锁、学习型索引（Kanva, arXiv 2308.11205 支持线性一致 range search） | 第 3 章待补充 / 第 6 章开放问题 | 可暂不实现 | — |

**建议的最小动作**：先做三项「零实现成本」的 P0（把第 4/6 章的「待补充」与「遗留问题」消化掉），再决定 v7 是做 KRL 还是做动态层级。后者更贴合本项目已有的「桶宽拐点」实验主线，工作量也更可控。

---

## 4. 三个可以直接写进文章的新论点

1. **「幻读防护」不等于「必须有 B+树有序区间」**。InnoDB 的 R-tree 空间索引明确因为没有全序而放弃 next-key，改用挂在索引页上的 MBR 谓词锁（`WL#6609`）。这把第 6 章的「索引依赖」从一句断言变成一个有生产实现支撑的边界条件，也能自然引出 predicate/precision lock 这一整支。
2. **锁数量与并发度之间存在第三种折中：资源映射**。Lomet 原文 §5.4 的结论是：range+key 合并能用一把锁保住最大并发，range+key+record 合并（ARIES/IM）锁开销最低但必然损失并发，且「靠发明新锁模式补不回来」。我们的 v4/v5 对比只测了「分离 vs 合并」的一个点，可以借此把分类表从两行扩成一行谱系。
3. **锁语义与锁服务是两个独立维度**。跨主机矩阵里 next-key 每事务 455.6 个 RDMA 操作、每把锁 5.6 个往返，是锁服务实现的代价；Sherman 的片上锁表 + 本地委托、DecLock 的队列通知、Lotus 的锁分片都在不改语义的前提下压这个数字。把这一维加进 `00-outline.md` 的分类表，能让 RDMA 章节从「实测报告」升格为「有坐标的分类学」。

---

## 5. 参考文献（按主题，含链接）

**经典 / 综述**
- Graefe, *A Survey of B-Tree Locking Techniques*, ACM TODS 35(3), 2010 — https://www.cs.berkeley.edu/~kubitron/courses/cs262a-F12/handouts/papers/a16-graefe.pdf
- Graefe, *Hierarchical Locking in B-Tree Indexes* — https://link.springer.com/chapter/10.1007/978-3-031-01873-2_3 ；专利 https://patents.google.com/patent/US20110208704A1/en
- Eswaran, Gray, Lorie, Traiger, *The Notions of Consistency and Predicate Locks in a Database System*, CACM 19(11), 1976: 624–633
- Jordan, Banerjee, Batman, *Precision Locks*, SIGMOD 1981: 143–147, DOI 10.1145/582318.582340 — https://portal.acm.org/doi/10.1145/582318.582340
- Gray, Lorie, Putzolu, Traiger, *Granularity of Locks and Degrees of Consistency in a Shared Data Base*, IFIP 1976（v6 已引）
- O'Neil, Fekete, O'Neil, Liarokapis, *PAX*, 2000 — https://www.semanticscholar.org/paper/e65b1db4865c07c00e08189f705394a45ed35d2d 🟡
- Lomet, *Key Range Locking Strategies for Improved Concurrency*, VLDB 1993: 655–664 — https://www.vldb.org/conf/1993/P655.PDF （镜像全文 https://bitsavers.trailing-edge.com/pdf/dec/tech_reports/CRL-93-2.pdf）
- Mohan, *ARIES/KVL*, VLDB 1990；Mohan & Levine, *ARIES/IM*, SIGMOD 1992 — 综述见 https://www.cs.cmu.edu/~natassa/courses/15-721/papers/p1-mohan.pdf §3.8
- Lomet 课程讲义（Fekete, EECS 262a）— https://people.eecs.berkeley.edu/~kubitron/courses/cs262a-F12/lectures/lec10-Isolation2.pdf

**工业实现**
- MySQL, *InnoDB Locking* §17.7.1 / *Locks Set by Different SQL Statements* §17.7.3 / *Phantom Rows* §17.7.4 — https://dev.mysql.com/doc/refman/8.4/en/innodb-locks-set.html
- MySQL WL#6609, *InnoDB GIS: Support Predicate Locking for GIS index* — https://dev.mysql.com/worklog/task/?id=6609
- Microsoft, *Key-Range Locking* — https://learn.microsoft.com/en-us/previous-versions/sql/sql-server-2008/ms191272(v=sql.100)
- Microsoft, `sys.sp_lock` 锁模式表 — https://learn.microsoft.com/en-ie/sql/relational-databases/system-stored-procedures/sp-lock-transact-sql
- PostgreSQL, `README-SSI` — https://git.postgresql.org/cgit/postgresql.git/tree/src/backend/storage/lmgr/README-SSI ；实现提交 https://git.postgresql.org/cgit/postgresql.git/commit/src/include?id=dafaa3efb75ce1aae2e6dbefaf6f3a889dea0d21
- CockroachDB, *Serializable, lockless, distributed: Isolation in CockroachDB* — https://www.cockroachlabs.com/blog/serializable-lockless-distributed-isolation-cockroachdb/
- Corbett et al., *Spanner*, OSDI 2012 §4.2.1 — https://www.researchgate.net/publication/262345649

**MVCC / OCC / 现代工作**
- Cahill, Röhm, Fekete, *Serializable Isolation for Snapshot Databases*, SIGMOD 2008
- Ports & Grittner, *Serializable Snapshot Isolation in PostgreSQL*, VLDB 2012 — https://www.eecs.umich.edu/courses/cse584/archive/fall2023/static_files/papers/snapshot-psql.pdf
- Neumann, Mühlbauer, Kemper, *Fast Serializable MVCC for Main-Memory Database Systems*, SIGMOD 2015 — https://www-db.cs.tum.edu/~muehlbau/papers/mvcc.pdf
- Yu et al., *TicToc*, SIGMOD 2016 — https://db.cs.cmu.edu/papers/2016/yu-sigmod2016.pdf
- Nakazono et al., *Griffin*, arXiv 2407.13294；Bessho & Kawashima, *Griffin-PPL*, CANDARW 2024, DOI 10.1109/CANDARW64572.2024.00015
- Kanva（无锁学习型索引、线性一致 range search）, arXiv 2308.11205 🟡
- Graefe et al., *Controlled Lock Violation*, SIGMOD 2013, DOI 10.1145/2463676.2465325

**RDMA / 分离式内存**
- Wang et al., *Sherman*, SIGMOD 2022 — https://arxiv.org/pdf/2112.07320 ；CACM 2025 版 https://mags.acm.org/communications/may_2025?folio=94
- Zhang et al., *DecLock*, arXiv 2505.17641 — https://arxiv.org/html/2505.17641v1
- *Lotus: Optimizing Disaggregated Transactions with Disaggregated Locks*, arXiv 2512.16136 — https://www.alphaxiv.org/pdf/2512.16136v1
- 中文综述《分离式内存索引系统研究综述》— https://dx.doi.org/10.7544/issn1000-1239.202550769

**结构层**
- Lehman & Yao, *Efficient Locking for Concurrent Operations on B-Trees*, TODS 1981 — https://dsf.berkeley.edu/jmh/cs262b/treeCCR.html
- TUM 课件《Synchronizing Data Structures》（OLC / B-link / Bw-tree / HTM）— https://www3.informatik.tu-muenchen.de/teaching/ws1718/dataprocessing/chapter3.pdf

---

## 6. 本轮未查证 / 待办

- PAX 的具体发表载体与页码（目前只有 Semantic Scholar 条目）。
- `docs/refs/references.bib` 目前不存在；若要正式引用，需新建文献库并按 `docs/README.md` 统一改用 `[@key]`。
- KRL 复合模式的完整兼容矩阵在原文 Table 9，PDF 为扫描 OCR，本次只核了模式名与文字结论，未逐格抄录；若写进正文需人工校对表格。
- Calvin / Aria 等确定性方案本轮未核原文，仅作方向占位。
- 「学习型索引上的范围锁」目前只找到 Kanva 等线性一致性 range search 工作，未找到直接讨论「间隙该如何定义」的文献 —— 第 6 章把它列为开放问题仍然成立。
