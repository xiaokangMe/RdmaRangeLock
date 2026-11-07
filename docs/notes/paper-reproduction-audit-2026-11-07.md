> 后续状态：本文是 5296bac 时点审计，下面的“当前/尚未”均指当时。真实邻接键 gap/next-key 修正及 prior-key 的后续实现见 classic-key-locks-implementation-2026-11-07.md；九模式、运行时升降和 DAG 的后续状态分别见 full-mgl-modes、bucket-runtime-conversion、dag-multipath-implementation 同日记录。旧实验结论仍按旧提交解释。

# 锁协议复现审计与统一架构适配准则（2026-11-07）

审查基线：9db05f7。本文记录该提交的代码事实，不修改历史实验数据，不以旧实验或本次回归检查证明新性能。

## 1. 结论与验收口径

本项目做的是“在同一索引、事务驱动、负载与本地/RDMA 后端上的论文机制复现”，不要求复刻完整数据库。允许改变锁表容器、通信方式、内存布局和写缓冲；必须保留或明确说明锁对象、兼容规则、访问覆盖、等待规则与释放时机。正确性需要不变式解释和可复现交错，不能仅靠补引用或跑通负载认定。

当前实现并非每个命令名都对应一种独立的已复现论文算法。可保留有依据的核心实现；gap/next-key 的邻接键机制尚不完整，不能将其现有性能归于 InnoDB、ARIES/KVL 或 Lomet KRL。此次没有把这些差异通过改名、贴引文当作已修复。

本次按用户补充的灵活复现标准审查：精确到一维谓词的实现是合法的受限复现；保守过锁可能不损害隔离，但若改变原算法的重要并发行为，就必须标注差异，不能用于证明原算法的优劣。

## 2. 实现—出处—差异表

| 当前入口/机制 | 可核验的依据 | 当前代码与适配 | 判定 |
|---|---|---|---|
| record-2pl / rdma-record-2pl | Eswaran1976 §2；Berenson1995 锁式隔离级别 | 点读 S、点写 X；扫描枚举记录后加 S 并复扫；写缓冲在提交时应用、随后统一解锁 | 保留。记录级严格两阶段锁基线；范围扫描不能防幻读，不与可串行化范围锁宣称同一隔离保证 |
| predicate / rdma-predicate | Eswaran1976 §3，印刷 pp.631–633，尤其公式 (13)(14)；Berenson1995 p.3 | S [lo,hi)，写 X point(k)，区间交且有写即冲突；事务结束释放 | 保留。单关系、唯一 uint64 键、whole-record、一维单区间谓词锁的受限复现；不是任意 SQL 谓词、空间谓词或 precision locking |
| exact-range / rdma-exact-range | 与上一行同一依据 | 与 predicate 相同的区间、模式、授权与释放，仅 kind/cache 标签不同 | 保留兼容入口，综述合并为同一个机制；不能算第 N+1 种算法或宣称独立锁表示优化 |
| gap-lock / rdma-gap-lock | Eswaran1976 谓词覆盖安全原则；Graefe2010 §5 的记录/空隙分离动机 | 先锁 (lo,hi)、point(lo)、point(hi) 的 Gap S，随后枚举并锁 Record S；插入 Gap X point，其他写 Record X point；种类之间仍做几何相交 | 当前是“逻辑查询区间保护 + 记录锁”的项目变体；有安全依据，尚非相邻现存键 gap 协议，更不是 InnoDB 纯抑制 gap 锁 |
| next-key / rdma-next-key | Graefe2010 §5.1–5.2；Mohan1990 ARIES/KVL 作目标对照 | point(lo)、扫描中 (prev,k]、末尾 (last,hi]；点操作仍用点锁；锁后复扫 | 当前是按扫描结果分段的逻辑谓词锁。lo/hi 未必为现存键，hi 不是 +∞；尚不能认定经典索引 next-key 已复现 |
| hierarchical 固定桶 | Gray1976 pp.365–370，Table 1 与 root-to-leaf rules；Graefe2010 §5.3 | root—固定等宽 bucket—record，IS/IX/S/X，scan+write 合为 X，无 SIX，先父后子，事务结束统一释放 | 保留。Gray 多粒度协议在固定分区资源树上的四模式保守适配；不是 Graefe separator-key，也不是“只有一层桶锁” |
| leaf hierarchy（实际祖先/叶对象） | Gray1976 的祖先意向协议；Deneva 源码的叶/父 CAS；Graefe2010 区分结构和事务保护 | 真实祖先 IS/IX、scan 叶 S、写 Record X，插入/删除持必要祖先 X；加锁后重新规划验证，严格持有 | 保留为项目的物理节点 MGL 适配。并非 Deneva 原样拷贝，更非 Graefe 的逻辑 separator-key 命名与分裂复制 |
| no-wait / wait-die / wound-wait | CMU2025 Two-Phase Locking slide 35（PDF p.85）；Spanner2012 §4.2.1 为 wound-wait 实例；Deneva 源码 | 老事务 wait-die 等待、年轻事务中止；wound-wait 老伤年轻、年轻等老；重试年龄不变；受伤方先清理；COMMITTING 不可再 wound | 保留为独立冲突处理维度，不计为额外范围锁算法；时间预算/最大重试不等于论文的无限重试活性保证 |
| v1/v2、RDMA 树、锁表与协程 | Graefe2010 locks/latches 分离；Deneva 执行流程 | 统一实验底座；本地 latch coupling、远端 offset 页与 RDMA 访问；区间槽和 gate 为项目实现 | 保留并独立报告工程代价；不是 B-link、OLC、Sherman HOCL、DecLock 或 LOTUS 的复现 |

Deneva 源码核验：pgm_deneva_backup，Git HEAD a1a2f4630f87753e5f419fb3bfd225178b11f48a；system/txn.cpp:2341 起 RDMA_WAIT_DIE、try_insert_new_data:5045 起父/叶 CAS。该工作区源码含 lock_type 的比较代替赋值、分裂分支提前 Abort 等现象，本次 system/txn.cpp 的 SHA-256 为 6938edda8d891e37003c6726fc8ed9d8708541c838551b7bbe286e32916c2710；Git HEAD 不代替工作区字节核验，后续直接复制必须记录文件哈希。源码只作为工程参考，不能代替论文出处，也不能为默认严格隔离引入已知错误。

## 3. 关键代码入口及正确性边界

- include/common/range.h：端点与 RangeLock::conflicts。普通区间按几何相交；Node 的意向例外只支持当前根/固定桶模型，不是任意层级的自动推理器。
- include/common/types.h：四模式兼容与合并。IS 与 IX 兼容；S 与 IX 不兼容；S+IX 向 X 保守合并，不能声称已实现 SIX 的并发度。
- src/txn/policy_workload.cpp::run_policy_workload：本地和 RDMA 共用的运行时语义，包括锁后复扫、own-write、prepare→apply_batch→unlock→end。
- src/txn/workload.cpp：保留的旧驱动，gap/next-key 使用相同的查询边界近似；不能只修新驱动而遗漏旧入口。
- src/txn/hierarchical_plan.h：固定分区资源树；src/txn/leaf_hierarchy.h：物理对象资源树和加锁后重新覆盖检查。
- src/lock/native_policy.cpp：年龄、attempt generation、wound/commit 仲裁。不得在受害者仍可写时偷走其物理锁。
- src/lock/lock_manager.cpp、remote_lock_manager.cpp、object_lock_manager.cpp：统一锁语义的本地/区间 RDMA/对象 RDMA 后端；布局差异不自动构成新的锁算法。
- src/index/rdma_btree.cpp：权威索引与记录均在远端；本地持临时副本，不是“远端只有锁表”。结构 latch 与事务 lock 的开销需分开观察。

### 3.1 为什么一维 predicate 可以保留

Eswaran 的单区间谓词 k>=lo AND k<hi 与写谓词 k=x 的可满足性交就是区间包含判定，不需要实现任意表达式求解器。§3 公式 (14) 给出相同关系、有写、谓词可同时满足三条件；p.633 还允许用更大的简单谓词保守覆盖。区间锁涵盖不存在的键，空结果也必须先锁后读。

前提：仅有一个关系/键空间；负载没有未建模的非键谓词；修改键值必须拆成旧键删除和新键插入并保护两边。当前 Operation 没有单步“修改主键”语义。

### 3.2 当前 gap 与 next-key 的真实区别

对非空查询 [lo,hi)，当前 gap 的三条查询保护锁的并集为 [lo,hi]。当前 next-key 的 point(lo)、中间分段、末尾区间的并集也为 [lo,hi]。两者额外锁住 hi；所有点写都与这些区间几何冲突。因此对一个已建立的扫描锁集合，两者对点写的覆盖基本相同，差异来自锁条目数、获取顺序/窗口、复扫和锁表组织。

这不证明两个完整事务驱动的调度、耗时相同，但足以说明它们尚不是经典“分离 gap/record”与“邻接键合并命名”全部差异的实验。给现有代码增加论文引用不能补齐缺失机制。

### 3.3 层级锁的正确解释

纯扫描可以取 root IS → bucket/leaf S；点写取 root IX → 同一 bucket/leaf IX → record X。阻塞扫描和写者的是覆盖对象上的 S/IX 冲突；IS/IX 不冲突。固定桶是合法资源树，但不是 B+ 树物理节点。当前叶路径虽按 B+ 树祖先命名，也不是逻辑 separator 区间命名。

默认 strict 路径的数据应用先于事务锁释放。hierarchy_behavior=deneva 的提前释放/部分插入例外只能独立标记，不能参加 strict 2PL 正确性结论。

## 4. 对已有调研报告的修正

1. 接受 predicate/exact-range 同语义的发现；不接受“一维区间没有一般 SQL 求解器，因此不是谓词锁机制复现”的推论。
2. InnoDB 不保存精确 WHERE 条件，不等于任何工业系统都做不到精确区间锁；精确锁也不是自动成立的吞吐上界或唯一最优基线。
3. 把精确谓词挂到节点作检索目录，不必改变逻辑冲突，也不必产生假阳性；把整个叶区间作为保护范围才会过锁。这是服务组织与锁语义两个维度。
4. Gray 允许人为定义资源层级，不要求资源树与 B+ 树同构；Graefe separator 锁是更具体的另一机制。
5. 现有 Node+KeyRange 例外不自动支持任意深度的逻辑重叠区间意向锁。新增 separator 层级必须先证明命名、跨层兼容与 SMO 的正确性。
6. 一般谓词可满足性不可判定、有限布尔表达式可满足性复杂、一维区间相交容易，是不同命题，不能混成“所有谓词判断 NP-hard”。
7. B-link/OLC 解决结构访问；SSI/MVCC/OCC/precision locking 各有验证与生命周期，不能用一个 S 区间锁代替它们就宣称已复现。
8. ARIES/KVL 是 C. Mohan，VLDB 1990，pp.392–405；不是 VLDB 1992。ARIES/IM 才是 Mohan 与 Levine，1992。
9. 共同实现瓶颈不保证各协议相对排名有效；不同锁数量/表示受到瓶颈影响不同，不能把项目排名直接归因到原论文。

## 5. 后续实现顺序与验收门槛

### P0：先完成经典邻接键范围锁，而不是继续增加名字

依据 Graefe2010 §5.1 的基本 next-key + 独立 key/gap 资源思想，在统一底座建立实际 predecessor/successor 与边界快照。查询边界不必存在，但锁的命名边界必须来自索引或真正的无穷哨兵。不能对两百万键全表扫描获取邻居，也不能为单一协议维护第二份索引。

1. 先给各索引后端设计统一的邻居/覆盖快照接口；不改变 IRangeLockManager 的公共签名，不把事务锁放进索引；若增加公共能力则独立接口里程碑。
2. 固定被复现的版本：基础 S/X next-key 可以先不引入 instant 或 KVL 全模式，但必须写明它较 KVL 更保守，不能称完整 KVL。gap/record 分离也必须选清普通冲突式资源还是 InnoDB 纯抑制式资源，不能混用。
3. 明确读、not-found、插入、删除、更新的锁对象；例如现存键 10/20/30，查询 [12,18) 不应继续造 12/18 为索引键边界，而应按照选定机制保护真实 (10,20) 或 (10,20]。
4. 完成锁前观察、获取锁、锁后重定位/验证；等待时不得持结构 latch。若锁名随插入/删除改变，应采用锁状态继承/迁移或稳定的显式区间保护并证明等价，不能漏掉 scan-to-lock 窗口。
5. 边界必须覆盖空树、最小键、最大键、末尾 gap、点 not-found、跨叶、删除边界、并发分裂与 ABA；+∞ 不是 uint64 最大值，更不是查询 hi。
6. 本地/真实 RDMA 上相同交错测试通过；先验正确性与压力验证达标，再安排正式矩阵。既有正式数据保持原 commit 和旧机制标签，不混合续跑。

### P1：层级协议保持正确，按研究问题增强

保留固定分区 MGL 与物理叶 MGL；SIX 是提高并发的可选机制，不是当前保守 X 错误的理由。若实现 Graefe separator/key-prefix，必须另做逻辑锁名、分裂复制/合并和层级兼容方案；先不把尚未实现的方案算进已复现数量。

### P2：扩展真正独立的机制

再考虑 Graefe 动态升级/降级、Lomet KRL 模式等。Precision/Griffin、SSI、B-link 或 RDMA 锁服务论文分别按其完整核心不变式推进，不通过换一个 LockKind 充数。

## 6. 本次已做与未做

已做：核对给定目录调研与原始文献相关页；建立本表与 BibTeX；修正第 4/5/6 章和 v4/v5 说明中的错误归因；在代码核心标注出处、适配与缺失机制；新增本地/RDMA 共用论文语义契约回归。

未做：没有修改 gap/next-key 授权算法冒称补齐经典实现；没有实现 SIX/instant/ghost/separator；没有重跑正式矩阵；没有改旧实验数值。验证：cmake 构建成功；真实 RDMA 回环服务下完整 CTest 48/48 通过，无跳过。日志 experiments/logs/paper-audit-20261107-074601/ctest.log。新增契约覆盖单区间/空结果防护、半开/无穷/最大键端点、读写两个到达方向、predicate/exact-range 跨标签冲突、Gray 四模式全兼容表、S/IX 子树保护、record-only 幻读反例。本表是机制审计与迁移依据，不是所有协议完整复现的验收证书。

## 7. 参考文献与具体依据

- [@eswaran1976] The Notions of Consistency and Predicate Locks in a Database System，CACM 19(11):624–633，§2–3，PDF pp.9–10 公式 (13)–(15)。https://doi.org/10.1145/360363.360369
- [@gray1976] Granularity of Locks and Degrees of Consistency in a Shared Data Base，Modelling in Data Base Management Systems，pp.365–394；PDF pp.4–5，Table 1、Rules for requesting nodes。http://jimgray.azurewebsites.net/papers/Granularity%20of%20Locks%20and%20Degrees%20of%20Consistency%20in%20a%20Shared%20Data%20Base.pdf
- [@graefe2010] A Survey of B-tree Locking Techniques，ACM TODS 35(3)，Article 16，§5.1–5.3，PDF pp.14–19。https://doi.org/10.1145/1806907.1806908
- [@mohan1990kvl] ARIES/KVL，VLDB 1990，pp.392–405，特别是 next-key、IX/SIX、instant 与 lock state replication。https://www.vldb.org/conf/1990/P392.PDF
- [@lomet1993] Key Range Locking Strategies for Improved Concurrency，VLDB 1993，pp.655–664。当前未实现其完整模式集。https://www.vldb.org/conf/1993/P655.PDF
- [@berenson1995] A Critique of ANSI SQL Isolation Levels，SIGMOD 1995，pp.1–10；pp.3–6 record/predicate、long duration 与 phantom。https://doi.org/10.1145/223784.223785
- [@innodb84locking] MySQL 8.4 §17.7.1 InnoDB Locking，Gap Locks / Next-Key Locks / Insert Intention Locks；官方快照 PDF pp.4–5。https://dev.mysql.com/doc/refman/8.4/en/innodb-locking.html
- [@cmu2025locking] CMU 15-445/645 Spring 2025，Two-Phase Locking，slide 35（合并 PDF p.85）。教学规则交叉校验，不冒充原始提出论文。
- [@corbett2012] Spanner: Google's Globally-Distributed Database，OSDI 2012，§4.2.1。https://www.usenix.org/conference/osdi12/technical-sessions/presentation/corbett

未取得全文的 Precision Locks/PAX/Griffin-PPL 不以题录页补写算法细节。Graefe 层级锁专利可支持 separator 方案研究，但两页章节节选不能当作完整论文。
