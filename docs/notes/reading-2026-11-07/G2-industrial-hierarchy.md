# G2 工业实现与层级/谓词粒度方向 · 精读笔记

> 供「范围查询锁」综述报告使用。负责编号：05, 06, 12, 24, 25, 26, 27, 28, 29, 31, 42, 43, 44（共 13 篇）。
> 全部结论均来自 `D:\codex\zongshu\.extract\` 下对应 txt 的实际文本，页码按 txt 内 `===== PAGE n =====` 标记标注。

## 阅读口径与文档性质说明

**类别粒度**：按用户要求，分类单元是「能作为一个实现版本、能用单变量实验对比」的方案，而不是论文或系统。因此 **gap lock 与 next-key lock 算同一类**（`C3`）：next-key 只是把「索引记录锁」与其「前驱 gap 锁」打包成一个命名资源，二者共享同一锁对象（相邻索引项之间的空隙），在单变量实验里自变量只是「是否锁洞 / 是否把 record+gap 合并命名」。同理，固定桶宽层级锁、separator-key 层级锁、key-prefix 锁虽然都叫「层级/粗粒度」，但是三个不同的锁命名方案（桶 id vs. 键值+层级 vs. 前缀串），各自是可独立实现的版本，故分开列。

**文档性质标注**：
- **官方文档快照**（网页打印 PDF）：24 / 25 / 26 / 27 / 28 为 Oracle/MySQL 官方站点打印快照（页脚带 `2026/11/7` 抓取时间与 `dev.mysql.com` URL）；06 为 IEEE Computer Society Digital Library 官方题录页打印快照；05 为 UC Berkeley 课程讲义网页打印快照；43 为 CMU 课程幻灯片打印快照。引文均注明「官方网页快照」。
- **正式出版物 PDF**：12（美国专利申请公开文本）、29（arXiv 论文）、42 / 44（ACM Computing Surveys）。
- **【非全文】**：06（`-record.pdf`，付费墙题录页，仅书目信息 + 可见摘要）、31（`-record.pdf`，Semantic Scholar 题录页，页面明写 `No Paper Link Available`，仅摘要 + 参考文献列表）。这两篇只提取书目信息与可见摘要，不推断正文机制。
- **抽取缺陷如实标注**：12 号 txt 的说明书正文抽取止于 `[0085]`（PAGE 20 末尾），**PAGE 21–25 为空白页**，故「key-prefix 锁」的详细文字段落与权利要求未落入 txt；本笔记对该部分的依据仅限 ABSTRACT（PAGE 1）、`[0005]`（PAGE 16）、图注 `[0020]–[0022]`（PAGE 16）与 FIG.12/13/14 图示（PAGE 13–15），不补写未抽取内容。

**与 RdmaRangeLock 的版本对照**（下文中 vN 指此）：v1 单线程 B+树、v2 latch coupling、v3 record-2PL、v4 gap lock、v5 next-key、v6 层级桶锁（固定桶宽 b16/b64/b256）；新增 `predicate`（一次范围操作一把整段区间 S 锁）、`exact-range`（精确 `[lo,hi)` 区间锁）、`rdma-predicate`。接口 `lock(mode, key_range)` / `unlock_all()` / `get_lock_count()` / `detect_conflict(other)`，严格 2PL，共用同一棵 B+树与 workload。

---

### 05 B+树 / GiST 的并发控制与幻读讲义（年份未标注；内容引用至 SIGMOD'97；2026-11-07 网页打印快照, UC Berkeley CS262b / `dsf.berkeley.edu/jmh/cs262b/treeCCR.html`）

- **一句话问题**：B+树查找只走 h 个页，用事务级 2PL 太重；而即使「树结构并发正确 + 记录 2PL」也仍挡不住「洞」里的插入，即幻读。
- **锁对象 / 粒度**：两条独立线索。①结构侧：`latch`（页/物理内存地址，短临界区）与 `lock`（事务可见数据、锁表命名、事务边界）严格区分；B-link tree 的 high key + right-link。②逻辑侧：`next-key` 锁「包含下一个更高值的记录」，或 `predicate` 锁整个查询谓词，或 GiST 里把谓词挂在被访问的索引节点上。
- **冲突判定**：结构侧靠 latch 与「自顶向下、自左向右」的全序避免死锁；逻辑侧 next-key 用哈希查锁名（"Cheap to check (hash lookup)"），predicate 锁需「顺序地把查询与所有锁逐一比较」。
- **关键机制**：Latch vs. Lock 对照表（PAGE 1）；Lehman/Yao B-link 搜索/插入算法（PAGE 2–3，`put(B,u)` 先于 `put(A,current)` 使分裂原子化）；**Degree-3 Consistency / Phantom Problem**（PAGE 4，例子 `name = "Dennis the Menace"` 首次查询为空、他人插入后重查出现 "Phantom Menace"）；三类解法对比（PAGE 4）；GiST/R-tree 的 **NSN（Node Sequence Number）** 分裂检测与右移界限（PAGE 4–5）；**把查询谓词 attach 到所有被搜索节点、 inserter 检查叶子上的谓词锁**，以及「Even simpler: overlock. Put "True" on all leaves you touch. GiST nodes become your physical surrogates!」；SMO 作为 nested top action 立即提交（PAGE 5）。
- **正确性**：树结构正确 + 记录 2PL 仍不足以防幻读——"2PL locks the data, but not the 'holes' in the data domain that overlap the query"（PAGE 4）。next-key 是「逻辑区间的物理替身」，是近似（"can be inaccurate"）；predicate 锁精确但对 `top n` 这类查询「not gradually expanding」。GiST 用节点的 physio-logical 层级做粗粒度替身防幻读。
- **代价**：latch 约为 lock 成本的 1/10（PAGE 1）；B-link 读完全不加锁（PAGE 2 "Requires NO locking for read (!!)"），写才偶发 lock coupling；predicate 锁「Expensive... requires sequentially comparing query against all locks」；GiST 的 overlock 用「精确度换廉价检查」，粒度可按实现精度逐级细化。
- **与 RdmaRangeLock 的关系**：**v2**（latch coupling / B-link）与 **v5**（next-key）的直接讲义依据；PAGE 4 的 `predicate` 与 PAGE 5 的「谓词挂节点 + 叶子 overlock」分别是 `predicate` 与 **v6 层级桶锁** 的思想原型；PAGE 6 明确说树层级「can be used for phantom protection instead of next-key locking」，是「层级范围锁」这一整条线的纲领句。
- **可复现性**：可在现有 B+树 + 固定接口下做。v2/v5 已实现；`predicate` 已实现；「谓词 attach 到内部节点」可补一个 `v6-attach` 版本，只需在 B+树遍历时记录路径节点，接口不变。
- **关键原文证据**（课程网页打印快照）：PAGE 4「**Next-Key Locking: queries and inserts try to lock the record in the DB that contains the next-higher value. A physical surrogate for a logical range. Cheap to check (hash lookup), can be inaccurate. Expands gradually on range search. This is what everybody does for B-trees.**」；PAGE 4「"Attach" query predicates to all nodes searched. Inserters check predicate locks attached to leaves that they visit.」；PAGE 6「Tree structure provides a "physio-logical" hierarchy ... can be used for phantom protection instead of next-key locking (another rough physical-to-logical mapping.)」；PAGE 1「Latches ... are on physical memory addresses (in bufPool) ... Locks are on externally visible data, and named in a lock table ... (10x latches).」

---

### 06 Bessho 等 · Griffin-PPL（2024, CANDARW 2024 / IEEE, pp. 36–42, DOI 10.1109/CANDARW64572.2024.00015）— **【非全文：付费墙题录页】**

> 文本为 IEEE Computer Society Digital Library 题录页打印快照（`computer.org/csdl/proceedings-article/...` 1/3–3/3），PAGE 1 含书目信息 + 摘要，PAGE 2 是 Similar Articles 列表，PAGE 3 是版权/页脚。页面显示 `Non-Member 36.00 / Member 16.00 / DOWNLOAD PDF`，正文不可见。以下只提取书目信息与可见摘要，不推断任何未展示的算法细节。

- **一句话问题**：Griffin 在 update-heavy 负载下，precision locking 维护的锁链表由一个 `sync manager` 线程统一释放，释放速度跟不上获取速度，导致锁表膨胀、吞吐骤降。
- **锁对象 / 粒度**（仅据摘要）：沿用 Griffin 的 **precision locking**——「protects updates and predicate reads with a lock」，即更新键与谓词读各持逻辑锁；具体锁名/区间形式摘要未给，不作推断。
- **冲突判定**（仅据摘要）：未展示。
- **关键机制**（仅据摘要）：提出 **Griffin-PPL（Parallel Precision Locking）**：去掉瓶颈线程 `sync manager`；把「释放锁」与「哈希表/B+-tree 同步」的工作并行化到全部 transaction worker 线程；为支持并行化，对锁链表采用**去中心化数据结构**（decentralized data structure for the lock lists）。
- **正确性**（仅据摘要）：目标是「prevents the lock lists from growing」；摘要未给出正确性论证或防幻读前提，不推断。
- **代价**（仅据摘要）：原架构的代价是锁链表长度增长导致 "severe performance degradation"；改进后微基准（50% inserts / 50% scans）在 **64 线程下吞吐为原 Griffin 的 6.5x**。
- **与 RdmaRangeLock 的关系**：对应 `predicate` / `rdma-predicate` 的「锁表可扩展性」章节，而非新的锁语义版本。它提示：`predicate` 版本若用集中式锁表，需要把 `get_lock_count()` 与锁表回收作为可测指标；与 29 号 Griffin 全文（Lu/Lp 为单条 lock-free 链表、tail 指针争用）互为因果，可合并为「precision locking 的中心化瓶颈」一条综述论点。
- **可复现性**：**不能单独立为一个锁语义版本**（无新锁对象/新冲突判定），但可作为 `predicate` 版本的一个「锁表实现」自变量（集中链表 vs. 分片/去中心化链表），在固定接口下可测。需要新增：多线程锁表 + 并发回收路径 + 线程数作为自变量。
- **关键原文证据**（IEEE 官方题录页快照，非全文）：PAGE 1「**Griffin is a hybrid transactional index structure that combines a hash table and B+-tree ... For serializable transactions, Griffin avoids phantoms with precision locking, a technique that protects updates and predicate reads with a lock.**」；PAGE 1「locks are released by only one thread called sync manager ... Griffin-PPL (Parallel Precision Locking) ... removes sync manager, the bottlenecking thread, and parallelizes the work of releasing locks and synchronization among all transaction worker threads ... we adopt a decentralized data structure for the lock lists.」；PAGE 1「under an update-heavy workload (50% inserts, 50% scans), Griffin-PPL exhibits a throughput 6.5x the conventional Griffin architecture, at 64 threads.」

---

### 12 Graefe · Hierarchical Locking in B-tree Indexes（2011 公开 / 2006-10-06 优先权, US 2011/0208704 A1, Microsoft Corporation）

> 美国专利申请公开文本（Inventor: Goetz Graefe, Bellevue WA；Assignee: Microsoft；Filed May 4 2011, Appl. No. 13/100,790；continuation of 12/499,624 → Pat. 7,953,717；continuation of 11/539,606 filed Oct. 6, 2006 → Pat. 7,577,658）。
> **抽取范围说明**：说明书正文抽到 `[0085]`（PAGE 20 末），PAGE 21–25 为空白，故 key-prefix 详细段落与权利要求不在 txt 中；本条对 key-prefix 的依据仅来自 ABSTRACT、`[0005]`、图注与 FIG.12/13/14 图示。

- **一句话问题**：传统 B+树层级锁「表/视图 → 索引或索引分区 → 叶页或单键」中间缺一级：索引太大时，要么一把全索引锁挡住上百并发事务，要么上百万把单键锁。
- **锁对象 / 粒度**：三个实施例共四类锁名。①单个键值；②键值与其后继键值之间的 **gap**；③key 与 gap 的**组合**；④**separator key**（内部索引节点中的 key-pointer pair，锁名 = `index id + key value + node level`）；⑤**key prefix**（键的前缀串，如 "JOHNSON"）。层级与 B+树高度对应：level 0 = leaf，level 1 = parent，依此类推。
- **冲突判定**：传统 `S/X/IS/IX` 兼容矩阵（FIG.4, PAGE 5）与新矩阵 500（FIG.5, PAGE 5，含 `S, X, SQ, QS, XQ, QX, SX, XS` 的 Y/N 表）。新锁模式把 key 与 gap 视为同一哈希条目的两个分量，`Q` 表示该分量为 null；「implied」意向锁施加在 key+gap 组合上，`SQ` 与 `QX` 可共存（一个锁 key 的 S、另一个锁 gap 的 X）。用 IS/IX/SIX 兼容矩阵判定层级冲突。
- **关键机制**：
  - **新锁模式**（`[0056]–[0063]`, PAGE 18–19）：`SQ / QS / XQ / QX / SX / XS`；因 key 值同时是 key、gap、key+gap 三资源在锁表哈希中的标识符，「only one resource and one invocation of the lock manager at the expense of additional lock modes」。
  - **插入（ghost + system transaction）**（`[0064]–[0071]`, PAGE 19–20）：system transaction 在相邻既有键上置 `QX` 锁住 gap，插入新键 + ghost record 后立即提交并释放 gap 锁；后续 user transaction 继承新键上的 `XQ`，把 ghost 改成 valid record。借此「avoid irregular complexities such as instant locks and range locks that do not fit the traditional theories of two-phase locking and of hierarchical locking」。
  - **separator key 层级范围锁**（`[0072]–[0078]`, PAGE 20，本组最核心）：锁一个 separator key = 锁「从该 separator（含）到下一个 separator（不含）」的整个半开键区间；父级锁等价于传统叶页锁，祖父级锁按扇出「may save tens or hundreds of locks」；最左子区间与最高键需 **fence key** 定界；**锁名是逻辑位置而非物理页 ID**，故分裂/合并/负载均衡导致 separator 迁移时锁仍有效。
  - **SMO 不变式**（`[0083]–[0084]`, PAGE 20）：分裂产生新 separator 时，把覆盖该区间的既有锁**复制**到新 separator 上，「no transaction's locked predicate is modified due to a change in the physical B-tree structure」；合并时反向合并锁集并校验无冲突（如 S 与 IX 不兼容则延迟合并）。
  - **动态粒度调整**（`[0045]–[0047]`, PAGE 18；FIG.9/10/11, PAGE 10–12）：`lock escalation`（结果集意外大时省 lock manager 调用与内存）、`lock de-escalation`（降争用，要求事务在私有内存保留足以便捷恢复细粒度锁的信息）、`granularity changes`；动机是「unpredictable concurrency contention」与「inaccuracy of cardinality estimation」。
  - **fence key 与零额外 I/O**（`[0048]–[0049]`, PAGE 18）：「for large index-order B-tree scans, the parent and grandparent nodes enable deep read ahead ... accessing those nodes and their separator keys does not incur any extra I/O」；每个分裂把 separator 副本（fence key）留在两个兄弟节点里。
  - **key prefix 锁**（仅 ABSTRACT / `[0005]` / FIG.12–14）：FIG.12 用 `SX`/`QX` 标注 `JOHNSON, SALLY` 与 `MILLER, STEVE` 的插入；FIG.13 标题 `LOCK ESCALATION`，把多个 JOHNSON 逐键锁升级为前缀 "JOHNSON" 的 S 锁；FIG.14 用 `LASTNAME, FIRSTNAME, MI` 三级键展示前缀层级。
- **正确性**：全部锁模式与层级都落在 2PL + 意向锁框架内（明确以「符合 two-phase locking 与 hierarchical locking 传统理论」为设计目标）；新锁模式下防幻读由「锁住 key 之间的开区间」保证；separator 锁通过「分裂时复制锁」保证物理结构变化不改变任何事务「locked predicate」的语义。
- **代价**：省下的是 lock manager 调用次数与锁表内存（祖父级一把顶父级几十上百把）；付出的是**新锁模式数量**（6 种）、fence key 存储、SMO 时的锁复制/合并冲突校验、de-escalation 需要在事务私有内存保留细粒度信息。粗粒度必然带来假冲突（锁整个 `[F...G)` 而非单个 "FRANKLIN"）。
- **与 RdmaRangeLock 的关系**：**这是 v6 的直接理论来源与主要对照物**，也是本组最需要展开的一条关系：
  - **v6 固定桶宽**（b16/b64/b256）＝ 在**键空间上静态等宽切分**，锁名是桶 id，桶边界与 B+树结构完全解耦，范围 `[lo,hi)` 映射为若干连续桶；优点是 SMO 无需迁移锁，缺点是边界固定、在偏斜数据上假冲突大、粒度不可随负载变化。
  - **Graefe separator-key 层级** ＝ 锁名是 `(index id, separator key, node level)`，**边界由 B+树内部节点真实 separator 决定并随树演化**；层级天然等于树高，扇出决定省锁倍数；用「逻辑命名 + 分裂时复制锁 + fence key」解决 SMO 一致性。它把 v6 的「一把粗锁」升级为「与树结构同构的一族粗锁」，粒度自适应、假冲突更小，但实现要处理 SMO。
  - **Graefe key-prefix 层级** ＝ 按**键值内容**（前缀串）而非键空间位置切分，粒度由前缀长度控制，可多级；适合字符串/复合键，与桶宽无关。
  - **动态升级/降级** ＝ v6 完全没有的维度：v6 的桶宽是实验前固定的常量，Graefe/CMU 的 escalation 是运行时按锁数量/争用阈值自适应。综述里应把「v6 固定桶宽」明确写成「Graefe 层级体系的一个**静态化、去结构化**简化」，把 separator / prefix / escalation 列为「可补的 v7/v8/v9」。
  - 新锁模式 `SQ/QS/XQ/QX/SX/XS` 与 v4/v5 属同一类（`C3`）的精细变体：v4 gap、v5 next-key 是「分开的两把锁」，Graefe 把它们合并成一条锁表条目的两个分量。
- **可复现性**：三类都能在现有 B+树 + 固定接口下做成单变量版本。①separator 层级：需新增「暴露内部节点 separator 键 + 节点 level」与「锁名 = (level, separator)」，自变量 = 锁所在层级（leaf/parent/grandparent）或树扇出；②key prefix：需新增键为字符串的 workload 与「前缀长度」自变量；③动态升级/降级：需新增 `get_lock_count()` 驱动阈值与事务私有细粒度信息缓存，`unlock_all()` 语义不变。新锁模式需把 `mode` 从 S/X 扩成 6 种并改 `detect_conflict(other)` 的兼容矩阵。
- **关键原文证据**（美国专利公开文本）：PAGE 1 ABSTRACT「**hierarchical lock modes are provided that permit locking a key, a gap between the key and the next key, and a combination of the key and the gap. In another example, key range locking may be applied to the B-tree index using locks on separator keys of index nodes. In another example, key range locking may be applied to the B-tree index using locks on key prefixes.**」；PAGE 16 `[0003]`「if a transaction already holds 10,000 locks and then escalates to an index lock to save on computing resources, this index lock might inhibit hundreds of concurrent transactions」；PAGE 19 `[0058]`「The new lock modes include key shared (SQ), gap shared (QS), key exclusive (XQ), gap exclusive (QX), key shared gap exclusive (SX), key exclusive gap shared (XS).」；PAGE 20 `[0073]`「**A lock on a separator key represents the entire key range from one separator key to its next neighbor but not including the neighbor.**」；PAGE 20 `[0075]`「Depending on a node's fan-out, a range lock at the grandparent level may save tens or hundreds of locks at the parent level」；PAGE 20 `[0077]–[0078]`「Locks on separator keys in parents are identified by the index identifier, the key value, and the node level within the B-tree ... **this identification of locks does not refer to a physical location ... but a logical location in the B-tree. Thus, locks remain valid even if a separator key migrates into a new node when a node splits**」；PAGE 20 `[0083]`「their locks on the range are duplicated and applied to the new separator key ... no transaction's locked predicate is modified due to a change in the physical B-tree structure.」；PAGE 18 `[0045]`「Lock escalation and de-escalation are useful due to unpredictable concurrency contention and due to inaccuracy of cardinality estimation during query optimization.」；PAGE 18 `[0048]`「accessing those nodes and their separator keys does not incur any extra I/O」；PAGE 10/11/12 图标题「LOCK ESCALATION / LOCK DE-ESCALATION / GRANULARITY CHANGES」。

---

### 24 MySQL 5.7.5 里程碑发布公告（2014-09-25, MySQL Blog Archive 官方网页快照，`dev.mysql.com/blog-archive/the-mysql-5-7-5-milestone-release-is-available/`，24 页）

- **一句话问题**：InnoDB 引入 R-tree/SPATIAL 索引后，多维数据没有全序，「next key」无法定义，必须换一套防幻读锁来支撑 REPEATABLE READ / SERIALIZABLE。
- **锁对象 / 粒度**：**predicate lock on MBR**（查询所用的 Minimum Bounding Rectangle / 最小外接矩形），锁对象是空间查询的矩形谓词本身，不是索引记录也不是 gap。
- **冲突判定**：其他事务「cannot insert or modify a row that would have matched the search condition」，即插入/修改行的 MBR 与已锁 MBR（或查询条件）相交即冲突。
- **关键机制**：`WL#6968`（Jimmy Yang，InnoDB R-tree 索引）——因「a search criteria could be met in multiple leaf pages of different search paths / a search/query bounding box can also intersect with or contain multiple leaf and non-leaf bounding boxes」，重构了 `row_search_for_mysql()` 与 `btr_cur_search_to_nth_level()`；`WL#6745`（Zheng Lai，R-tree split/shrink + 全部 MBR 操作函数）；`WL#6609`（Jimmy Yang，predicate locking）；`WL#7928` GeoHash——「make it possible to index and search for geographical data in one-dimensional (B-tree) indexes」。谓词被限定为空间查询里的矩形（`ST_Within` / `ST_Contains`）。
- **正确性**：用于「enforce consistent read」、防 phantom；仅在 REPEATABLE READ / SERIALIZABLE 需要，因为 next-key 在这两个级别失效。前提是谓词被限制为 MBR 矩形，Blog 自述「a fairly specialize 'predicate' locking」（见 28 号 worklog 原文）。
- **代价**：24 号 Blog 未给量化代价；只说明 R-tree 遍历与 B-tree 不同（多路径、多 bounding box 相交），因此不能复用 B+树的 next-key 机制；28 号 worklog 补充「predicate locking is its overhead and being expensive」。
- **与 RdmaRangeLock 的关系**：`predicate` / `rdma-predicate` 的工业出处。关键论点是**「无全序 ⇒ next-key 不可定义 ⇒ 退回谓词锁」**：这正好解释了为什么 `predicate` 版本不能只靠 v5 的「后继记录」实现，也解释了 MBR 与 `key_range` 的关系——一维键空间上，MBR 退化为 `[lo,hi)` 区间锁，即 `exact-range` / `predicate`；`WL#7928` 的 GeoHash 则给出「把 2D 谓词编码成 1D 键再套 B+树区间锁」的可复现桥。
- **可复现性**：一维退化版（MBR → `[lo,hi)`）已由 `predicate` / `exact-range` 覆盖，无需新增；真·二维 MBR 在现有单维 B+树 + `key_range` 接口下**不能直接做**，需新增二维键/矩形类型与矩形相交判定（或新增空间填充曲线编码）。可作为「谓词表达能力」单变量：1D 区间 vs. 2D 矩形。
- **关键原文证据**（MySQL 官方博客网页快照）：PAGE 10–11「We have implemented spatial indexing in InnoDB ... **InnoDB spatial index supports full transaction properties, as well as isolation levels. It employs predicate lock to prevent the phantom scenario.**」；PAGE 11「**For InnoDB spatial indexes we cannot use next key locking to support the REPEATABLE READ or SERIALIZABLE isolation levels as there is no such absolute ordering concept in multi-dimensional data. So it is difficult to define the "next" key.** Instead, this work uses predicate locks ... we might simply lock the Minimum Bounding Rectangle (MBR) which was used for the query, and other transactions cannot insert or modify a row that would have matched the search condition.」（续至 PAGE 12）；PAGE 12「The predicate in this case is limited to the MBR used in the spatial query (e.g. ST_Within or ST_Contains).」；PAGE 13「GeoHash support (WL#7928) ... make it possible to index and search for geographical data in one-dimensional (B-tree) indexes.」

---

### 25 MySQL 8.4 · 17.7.1 InnoDB Locking（MySQL 8.4 Reference Manual, 2026-11-07 官方文档网页快照，`dev.mysql.com/doc/refman/8.4/en/innodb-locking.html`，7 页）

- **一句话问题**：列清 InnoDB 的锁类型，并说明在哪些隔离级别/索引条件下会触发 gap / next-key / predicate 锁。
- **锁对象 / 粒度**：多粒度层级（表 → 行/索引记录）。类型清单（PAGE 1）：S/X 行锁、Intention Locks（IS/IX，表级）、Record Locks（索引记录）、Gap Locks（索引记录之间的间隙，或首记录之前/末记录之后）、Next-Key Locks、Insert Intention Locks、AUTO-INC Locks、**Predicate Locks for Spatial Indexes**。
- **冲突判定**：`S/X` 标准矩阵；IS/IX/S/X 表级兼容矩阵（PAGE 2，如 `IX` 与 `IS` 兼容、`X` 与一切冲突）；**gap 锁特殊**：「purely inhibitive」，「There is no difference between shared and exclusive gap locks. They do not conflict with each other」——多个事务可对同一 gap 同时持有冲突名义的 gap 锁（PAGE 4）；insert intention 之间若不插同一位置则互不阻塞。
- **关键机制**（工业文档，写清触发条件）：
  - Record lock「always lock index records」，无索引时 InnoDB 建隐藏聚簇索引（PAGE 3）。
  - `SELECT c1 FROM t WHERE c1 BETWEEN 10 and 20 FOR UPDATE` → 锁住范围内所有既有值之间的 gap，阻止插入 15（PAGE 3）。
  - **唯一索引 + 唯一搜索条件** → 只要 index-record lock，**不加 gap 锁**（PAGE 3–4）；非唯一索引或范围条件 → 加 gap / next-key（见 26）。
  - next-key 区间示例（索引含 10,11,13,20）：`(−∞,10] (10,11] (11,13] (13,20] (20,+∞)`，末区间锁 supremum 伪记录之上的 gap（PAGE 4–5）。
  - **默认 REPEATABLE READ**：「InnoDB uses next-key locks for searches and index scans, which prevents phantom rows」（PAGE 5）。
  - **READ COMMITTED**：「Gap locking can be disabled explicitly ... gap locking is disabled for searches and index scans and is used only for foreign-key constraint checking and duplicate-key checking」（PAGE 4）。
  - **SPATIAL 索引**：「next-key locking does not work well to support REPEATABLE READ or SERIALIZABLE ... so it is not clear which is the "next" key ... InnoDB uses predicate locks. A SPATIAL index contains minimum bounding rectangle (MBR) values ... setting a predicate lock on the MBR value used for a query」（PAGE 7）。
- **正确性**：REPEATABLE READ 下 next-key 防幻读；gap 锁设计成「纯抑制插入」而非互斥，因此正确性依赖「插入必须越过 gap 锁」而非「gap 锁互斥」；禁用 gap 锁会产生幻读（见 27 PAGE 2）。
- **代价**：next-key 每记录一把锁且覆盖前驱 gap → 假冲突（锁了并不存在的区间）；意向锁引入表级锁表条目；supremum 伪记录是额外边界设施；多粒度层级本身是为减少锁数量（见 43）。
- **与 RdmaRangeLock 的关系**：**v3（record-2PL）、v4（gap）、v5（next-key）三类共享同一份工业规格**，也是「gap 与 next-key 算同一类」的最强证据：官方定义 next-key = record lock + 前驱 gap lock（PAGE 4）。`predicate` / `rdma-predicate` 对应 PAGE 7 的 MBR predicate lock。意向锁层级对应 v6 的层级设计。
- **可复现性**：v3/v4/v5 已实现；「唯一索引唯一搜索 → 退化为纯 record lock」「READ COMMITTED → 关闭 gap」是两个可在固定接口下做的单变量（布尔开关），无需新增接口；MBR 谓词锁的一维退化即 `predicate`。
- **关键原文证据**（MySQL 8.4 官方文档快照）：PAGE 4「**A next-key lock is a combination of a record lock on the index record and a gap lock on the gap before the index record.**」；PAGE 4「**Gap locks in InnoDB are "purely inhibitive", which means that their only purpose is to prevent other transactions from inserting to the gap. ... There is no difference between shared and exclusive gap locks. They do not conflict with each other, and they perform the same function.**」；PAGE 5「By default, InnoDB operates in REPEATABLE READ transaction isolation level. In this case, InnoDB uses next-key locks for searches and index scans, which prevents phantom rows」；PAGE 4「Gap locking can be disabled explicitly. This occurs if you change the transaction isolation level to READ COMMITTED.」；PAGE 7「**To handle locking for operations involving SPATIAL indexes, next-key locking does not work well ... There is no absolute ordering concept in multidimensional data, so it is not clear which is the "next" key. ... InnoDB uses predicate locks. A SPATIAL index contains minimum bounding rectangle (MBR) values**」；PAGE 2 IS/IX/S/X 兼容矩阵。

---

### 26 MySQL 8.4 · 17.7.3 Locks Set by Different SQL Statements in InnoDB（MySQL 8.4 Reference Manual, 2026-11-07 官方文档网页快照，`dev.mysql.com/doc/refman/8.4/en/innodb-locks-set.html`，6 页）

- **一句话问题**：把「哪条 SQL 在哪种索引/隔离级别下加哪把锁」逐条落表。
- **锁对象 / 粒度**：索引记录（record lock）、gap、next-key；secondary index 上的锁还会回取聚簇索引记录再加锁；无合适索引时退化为全表每行加锁。
- **冲突判定**：沿用 S/X 与 gap 的「纯抑制」语义；INSERT 的 insert intention gap lock 只与「同位置插入」冲突。
- **关键机制**（工业文档核心，逐条触发条件）：
  - 「A locking read, an UPDATE, or a DELETE generally set record locks on **every index record that is scanned** ... **It does not matter whether there are WHERE conditions** ... **InnoDB does not remember the exact WHERE condition, but only knows which index ranges were scanned**」（PAGE 1）——即锁的是「扫描过的索引范围」，不是精确谓词。
  - `SELECT ... FROM` 快照读不加锁；**SERIALIZABLE** 时「sets shared next-key locks on the index records it encounters」（PAGE 1）。
  - 「For a **unique index with a unique search condition**, InnoDB locks only the index record found, **not the gap before it**」；「For other search conditions, and for non-unique indexes, InnoDB locks the **index range scanned**, using gap locks or next-key locks」（PAGE 2）。
  - `UPDATE ... WHERE ...` / `DELETE FROM ... WHERE ...` → 「sets an exclusive next-key lock on every record the search encounters」（PAGE 2）。
  - `INSERT` → 「an exclusive lock on the inserted row. This lock is an **index-record lock, not a next-key lock** (that is, there is no gap lock)」；插入前先置 insert intention gap lock（PAGE 2）。
  - `INSERT ... ON DUPLICATE KEY UPDATE` → 重复唯一键时取 **exclusive next-key lock**；`REPLACE` 冲突时 likewise；`INSERT INTO T SELECT ... FROM S` 非 READ COMMITTED 时对 S 的行设 **shared next-key locks**（PAGE 4）。
  - 「All InnoDB locks held by a transaction are released when the transaction is committed or aborted」（PAGE 6）——严格 2PL 的释放点。
- **正确性**：防幻读依赖「锁住扫描到的整个索引范围 + gap」，而非 WHERE 条件谓词；因此同一 WHERE 但不同可用索引会导致加锁范围不同，这是 known 的过锁来源。
- **代价**：**不记精确 WHERE 条件** → 锁数量 = 扫描过的索引记录数（而非结果行数）；无索引时全表锁；假冲突与额外遍历来自「扫描范围 ⊇ 结果范围」。
- **与 RdmaRangeLock 的关系**：这是 `exact-range` 版本的**反面对照**——`exact-range` 精确锁 `[lo,hi)`，而 InnoDB「does not remember the exact WHERE condition, but only knows which index ranges were scanned」，实际是「按 B+树遍历足迹」加锁，更接近 v5 next-key 与 v6 桶锁的混合。综述里可用它论证「精确区间锁 vs. 索引范围锁」是两个不同类别（`C4` vs `C3`）。
- **可复现性**：可在固定接口下做两个单变量版本：①「按结果集精确区间加锁」（`exact-range`）vs.「按扫描足迹加锁」（v5/v6）；②「唯一索引唯一查找」开/关 gap。无需新增接口，只需让 workload 携带索引可用性标记。
- **关键原文证据**（MySQL 8.4 官方文档快照）：PAGE 1「**InnoDB does not remember the exact WHERE condition, but only knows which index ranges were scanned.**」；PAGE 2「For a unique index with a unique search condition, InnoDB locks only the index record found, not the gap before it. **For other search conditions, and for non-unique indexes, InnoDB locks the index range scanned, using gap locks or next-key locks**」；PAGE 2「UPDATE ... WHERE ... sets an exclusive next-key lock on every record the search encounters.」「INSERT sets an exclusive lock on the inserted row. This lock is an index-record lock, not a next-key lock」；PAGE 6「All InnoDB locks held by a transaction are released when the transaction is committed or aborted.」

---

### 27 MySQL 8.4 · 17.7.4 Phantom Rows（MySQL 8.4 Reference Manual, 2026-11-07 官方文档网页快照，`dev.mysql.com/doc/refman/8.4/en/innodb-next-key-locking.html`，2 页）

- **一句话问题**：用最小例子说明幻读为何发生、next-key 如何「锁住不存在的东西」。
- **锁对象 / 粒度**：索引记录 + 其前驱 gap（next-key）；额外还有「最后一条记录之后的 gap」。
- **冲突判定**：「If one session has a shared or exclusive lock on record R in an index, another session cannot insert a new index record in the gap immediately before R」。
- **关键机制**（工业文档）：例子 `SELECT * FROM child WHERE id > 100 FOR UPDATE`，表中有 90 与 102；`id` 有索引，扫描从第一个 >100 的记录开始；若「the locks set on the index records in the scanned range do not lock out inserts made in the gaps (in this case, the gap between 90 and 102)」，另一会话可插入 101，重跑同一 SELECT 得到 phantom（PAGE 1）。「**To prevent phantoms, InnoDB uses an algorithm called next-key locking that combines index-row locking with gap locking**」；且「**When InnoDB scans an index, it can also lock the gap after the last record in the index** ... the locks set by InnoDB include a lock on the gap following id value 102」（PAGE 1）。唯一性检查用法：以 share mode 读未见重复 → 后继记录上的 next-key 锁阻止他人插入重复（PAGE 1–2）。
- **正确性**：REPEATABLE READ 下 next-key 防幻读；「**next-key locking enables you to "lock" the nonexistence of something in your table**」（PAGE 2）；禁用 gap 锁（READ COMMITTED）「may cause phantom problems」（PAGE 2）。
- **代价**：必须锁「最后一条记录之后的 gap」（supremum 侧）才能挡住上界外插入，说明「洞」的边界处理是额外设施；锁数量随扫描记录数线性增长。
- **与 RdmaRangeLock 的关系**：**v5 的教科书级正确性论据**；「gap after the last record」对应 `exact-range` 的上界开区间 `[lo,hi)` 中 `hi` 之外/之内的边界语义，是设计 `detect_conflict` 时要处理的「末端洞」。
- **可复现性**：已由 v4/v5 覆盖；可加一个单变量「是否锁 supremum gap」，观察上界外插入能否造成幻读。
- **关键原文证据**（MySQL 8.4 官方文档快照）：PAGE 1「**To prevent phantoms, InnoDB uses an algorithm called next-key locking that combines index-row locking with gap locking.**」；PAGE 1「When InnoDB scans an index, it can also lock the gap after the last record in the index ... the locks set by InnoDB include a lock on the gap following id value 102.」；PAGE 2「Thus, the next-key locking enables you to "lock" the nonexistence of something in your table.」；PAGE 2「Gap locking can be disabled ... This may cause phantom problems」。

---

### 28 MySQL WL#6609 · InnoDB GIS: Support Predicate Locking for GIS index（年份未标注；MySQL Worklog 官方网页快照，`dev.mysql.com/worklog/task/?id=6609`，1 页，Status: Complete）

- **一句话问题**：BUG#25847「ADD PREDICATE LOCKING TO AVOID DEADLOCKS DUE TO LOCKING NON-EXISTENT ROWS」——R-tree 无全序，gap/next-key 锁「不存在的行」会引发死锁，改用谓词锁。
- **锁对象 / 粒度**：查询所用的 **MBR（minimum bounding rectangle/box）**。
- **冲突判定**：「other transactions cannot insert or modify a row that would have matched the condition」——插入/修改行若命中已锁 MBR 对应条件即冲突。
- **关键机制**（工业文档）：「For InnoDB GIS index, it is difficult (if not impossible) to use the GAP lock (or next key locking) to support the "repeated read" or "serializable read" isolation level, since **there is no such absolute ordering concept in multi-dimension data. So it is difficult to define the "next" key**」；「We proposed to use "Predicate lock" ... **we might simply lock the MBR** ... The predicate in this case might be limited to rectangle used in GIS query. So it is a fairly specialize "predicate" locking」；**关键的层级化改进**：「The problem with predicate locking is its overhead and being expensive. We might need to **improve the granularity of predicate locking by "attaching" the predicates to GIS index nodes. So that queries do not touch related nodes (or area) will not be evaluated against such predicates**」（PAGE 1）。
- **正确性**：目标是 REPEATABLE READ / SERIALIZABLE 下的一致读；前提是谓词被 specialize 成矩形 MBR。
- **代价**：明说谓词锁「its overhead and being expensive」；改进方向是「把谓词挂到索引节点上」，让不相关的节点/区域不参与谓词求值——即用索引层级做谓词锁的粗粒度剪枝。
- **与 RdmaRangeLock 的关系**：**把 05 号讲义「attach predicates to nodes」、12 号 separator 层级锁、29 号 precision locking 串起来的工业桥梁**。它证明「谓词锁 + 索引节点层级」是同一个设计动机的两种表述：`predicate` / `rdma-predicate` 是「一把整段区间锁」，而「attach 到节点」正是 v6 层级桶锁在谓词语境下的等价物。综述可据此把「谓词锁的粒度问题」与「v6 桶宽问题」统一为同一个自变量。
- **可复现性**：可做。在现有 B+树 + 固定接口下，`predicate` 已是「一把区间锁」；再加一个「把谓词拆到 B+树内部节点 / 固定桶上」的版本，自变量 = 谓词挂载层级（整棵树 / 内部节点 / 叶 / 桶），观察冲突检查次数与假冲突率。只需暴露遍历路径节点。
- **关键原文证据**（MySQL 官方 Worklog 快照）：PAGE 1「**For InnoDB GIS index, it is difficult (if not impossible) to use the GAP lock (or next key locking) to support the "repeated read" or "serializable read" isolation level, since there is no such absolute ordering concept in multi-dimension data. So it is difficult to define the "next" key.**」；PAGE 1「we might simply lock the MBR (minimum bounding rectangle/box) which is used for the query, and other transactions cannot insert or modify a row that would have matched the condition ... So it is a fairly specialize "predicate" locking.」；PAGE 1「**The problem with predicate locking is its overhead and being expensive. We might need to improve the granularity of predicate locking by "attaching" the predicates to GIS index nodes.**」

---

### 29 Nakazono 等 · Griffin: Fast Transactional Database Index with Hash and B+-Tree（2024, arXiv:2407.13294v1 [cs.DB], 18 Jul 2024）

- **一句话问题**：B+树的点查是 O(log N) 而哈希表可 O(1)；同时防幻读的 next-key 与 rescanning 都要「每记录两把锁」或「双倍 B+树遍历」。能否两者都省？
- **锁对象 / 粒度**：**precision locking** 的两个逻辑集合：**Lu** = 更新键集合（Insert / Delete 的单个 key），**Lp** = 读谓词集合（Scan 的单个 key range）。锁是「与所访问的 key（或 key range）关联的逻辑锁」，不是索引结构上的锁；`Scan` 的谓词被假定为「a single key range」。
- **冲突判定**：Insert/Delete 把 key 加入 Lu 后检索 Lp，若该 key 落在任一已登记 range 内则 abort；Scan 把 `[begin,end)` 加入 Lp 后检索 Lu，若任一 key 落在该 range 内则 abort（Alg.2 PAGE 6）。**双向、区间重叠式判定**，与 RdmaRangeLock 的 `detect_conflict(other)` 同构。
- **关键机制**：
  - 幻读避免技术分类（PAGE 3）：**next-key locking**（MySQL/DB2，「acquires two locks for each record being accessed」，低并发）、**rescanning**（Hekaton/Silo，commit 时重扫验证，「doubles the calculation costs of scans」）、**precision locking**（Hyper/AOCC；是 predicate locking [Eswaran 1976] 的扩展，「reduces the computational cost by limiting the operation set to Insert, Delete and Scan, and **predicates to ranges**」，PAGE 9）。
  - 混合架构：hash table（O(1) 点操作，永远最新）+ B+-tree（Scan）；更新先落 hash，再由 **sync manager** 周期性批量应用到 B+树；**precision locking 同时承担 linearizability**：未同步到 B+树的 Lu 条目会让重叠 Scan 直接 abort（PAGE 4–5）。
  - Lock 生命周期：「acquired before the actual insert/delete/scan, and released after the transaction that issued the operation terminates」；Lu/Lp 条目带 `active/committed/aborted` 状态；GC 两条不变式（PAGE 5）：①条目仅在创建事务终止后删除；②committed 的 Lu 条目必须在其 Insert/Delete 已应用到 B+树后才能删除。
  - 实现：Lu/Lp 为 **lock-free 单链表 + tail 指针**，CAS 追加、从 tail 遍历整表做冲突检查（PAGE 7）；Bw-tree 只存 key，值在 hash table（PAGE 7）。
- **正确性**：同时保证 **phantom avoidance** 与 **linearizability**（PAGE 2、5）；FIG.4 给出两类 abort 例子：(a) 幻读、(b) 陈旧读。前提是「每个 Scan 只有一个 key range」这一谓词限制。
- **代价**：Scan 代价 `O(|Lu|) + O(log N) + O(M)`（Tab.III PAGE 4）；冲突检查需遍历整条锁链表；**Insert-only 高线程下 Lu 单链表 tail 指针成为集中瓶颈，80 线程时比 baseline 差约 1.9x**（PAGE 8），作者自己指出「By decentralizing the Lu currently implemented in the single lock-free list ... Griffin would alleviate the contentions」——这正是 06 号 Griffin-PPL 解决的问题。评测口径：「we only measure its performance where only the phantom anomaly is avoided」「We simulate short, single-operation transactions」（PAGE 7）。
- **与 RdmaRangeLock 的关系**：`predicate` / `rdma-predicate` 的现代复活范本。**Griffin 的 Lp 条目 = `predicate` 的「一次范围操作一把整段区间 S 锁」**；Lu 条目 = 写侧的 key 锁；`precision locking` 的「谓词限定为 range」正是 `key_range` 接口能承载的形式。29 号 PAGE 10 明说「precision locking is oblivious to the index data structure, we can easily use any ordered concurrent index structure, including a skip list, LSM-tree, etc.」——即它可直接套在共用 B+树上。与 v5 的差异：v5 把锁挂在「索引记录/后继」上，precision locking 把锁挂在「逻辑区间」上，因此不需要额外 B+树遍历（对应 `predicate` 相对 next-key 的优势）。
- **可复现性**：**可以**，且是本组最容易做成单变量版本的一类。在固定接口下：`lock(S, [lo,hi))` 记入 Lp，`lock(X, [k,k])` 记入 Lu，`detect_conflict` 做「区间重叠 + key∈range」；`unlock_all()` 对应 Commit/Abort 时标记 + GC。需要新增：①写侧「单键」到 `key_range` 的退化表示；②锁表/conflict 检查的组织方式（线性链表 vs. 哈希/区间树）作为自变量；③（可选）sync manager 与 B+树异步同步——但严格 2PL + 共用 B+树的项目设定下可省略这一步，只保留 Lu/Lp 语义。
- **关键原文证据**（arXiv 论文）：PAGE 1 Abstract「**For phantom avoidance, Griffin employs a precision locking method, which does not involve additional traversal of the B+-tree.**」；PAGE 3「In precision locking, Inserts, Deletes, Scans, i.e., operations that can cause phantoms, are guarded with a logical lock associated with the key (or key range) they access.」+ Tab. II「**Lu: Set of updated keys (Inserts and Deletes) / Lp: Set of read predicates (Scans)**」；PAGE 3「We (and [9]) assume each Scan has a read predicate that is a single key range. An Insert or Delete first adds its entry ... to Lu, and searches Lp to detect any range that overlaps with the inserted or deleted key.」；PAGE 6 Alg.2「if key is in some range in Lp then return abort」「if some key in Lu is in begin-end then return abort」；PAGE 9「**predicate locking ... has not been used in practice because detecting conflicts between general predicates is NP-hard. Precision locking [12] reduces the computational cost by limiting the operation set to Insert, Delete and Scan, and predicates to ranges.**」；PAGE 8「adding entries to Lu severely limits efficiency because all the concurrent Inserts collide with the tail pointer of the same linked list」；PAGE 10「Since precision locking is oblivious to the index data structure, we can easily use any ordered concurrent index structure」。

---

### 31 O'Neil / Fekete / Liarokapis · PAX（2000, "A New Hybrid Predicate-Assertion-Index-Locking (PAX) Algorithm to Prevent Transactional Phantoms", Corpus ID 15114770）— **【非全文：题录页】**

> 文本为 Semantic Scholar 题录页打印快照，PAGE 1 明写 **"No Paper Link Available"**；PAGE 1 含标题/作者/年份/摘要/TLDR，PAGE 2–3 为参考文献列表，PAGE 4–5 为站点页脚。以下只提取书目信息与可见摘要/参考文献，不推断正文算法。

- **一句话问题**（仅据摘要）：在 KVL/IM 锁之上，用另一种防幻读方法在「certain application regimes」下换取更高并发。
- **锁对象 / 粒度**（仅据摘要）：称为 **PA（Predicate-Assertion）**——「"asserts" read predicates and updates in memory」，基于 **precision locking**；混合对象为 KVL/IM 的索引锁 + 内存中断言的读谓词/更新。具体锁名与粒度摘要未给，不推断。
- **冲突判定**（仅据摘要）：未展示。
- **关键机制**（仅据摘要）：「A novel and workable extension of KVL/IM locking ... an alternative phantom prevention method that "asserts" read predicates and updates in memory, a technique based on precision locking which is called PA」。标题里的 Hybrid = KVL/IM（索引区间锁） + PA（内存谓词断言）。参考文献列表可见其学术谱系：Eswaran/Gray/Lorie/Traiger 1976 *predicate locks*、Jordan/Banerjee/Batman 1981 *Precision locks*、Mohan 1990 *ARIES/KVL*、Mohan & Levine 1992 *ARIES/IM*、Berenson 1995 *A critique of ANSI SQL isolation levels*、Gray & Reuter 1992。
- **正确性**（仅据摘要）：未给出正确性论证；仅由参考文献中「transactions under PL are well formed and two phased」可推知 PL 与 2PL 相容，但这属于参考文献（PAGE 2）而非本文正文，标注为「据题录页所列参考文献」。
- **代价**（仅据摘要）：未展示；参考文献页对 Precision locks 的描述称「PL usually is cheaper to execute」（PAGE 3 续 PAGE 2 条目），同样属参考文献而非本文。
- **与 RdmaRangeLock 的关系**：**时间线锚点**，把 1976 predicate locks → 1981 precision locks → 1990 ARIES/KVL → 2000 PAX → 2024 Griffin 串成一条线；对应 `predicate` 与 v5 的「混合」思路（KVL/IM 的索引区间锁 ≈ v5/v6，内存谓词断言 ≈ `predicate`）。因非全文，**不引用其具体机制**，只作谱系与「precision locking 早在 2000 年就被用于混合防幻读」的论据。
- **可复现性**：**不能据该文档复现**（无全文、无算法细节）。可复现的对应物是 29 号 Griffin 的 Lu/Lp 与 `predicate` 版本；若综述需要「KVL + precision 混合」版本，应基于 29 号与 03/04 号（不在本组）自行设计，不能声称来自 31 号。
- **关键原文证据**（Semantic Scholar 题录页快照，非全文）：PAGE 1「**A novel and workable extension of KVL/IM locking is introduced, to provide increased concurrency in certain application regimes by an alternative phantom prevention method that "asserts" read predicates and updates in memory, a technique based on precision locking which is called PA.**」；PAGE 1「P. O'Neil, A. Fekete, D. Liarokapis • Published 2000」「**No Paper Link Available**」；PAGE 2「Precision locks / J. Jordan J. Banerjee R. Batman • ACM SIGMOD Conference • 1981 / It is shown that transactions under PL are well formed and two phased」；PAGE 3「The notions of consistency and predicate locks in a database system / K. Eswaran Jim Gray R. Lorie I. Traiger • CACM • 1976」。

---

### 42 Bernstein & Goodman · Concurrency Control in Distributed Database Systems（1981, ACM Computing Surveys, Vol. 13, No. 2, June 1981；PAGE 1 页脚版权年 1981、PAGE 11 页眉页码 195）

- **一句话问题**：把分布式并发控制问题分解为 read-write / write-write 两个同步子问题，统一描述 48 种方法，并给出 2PL 的形式化正确性。
- **锁对象 / 粒度**：抽象的「data item x」及其 readlock / writelock；Basic 2PL 把 scheduler 放在存储 x 的 DM 上，逻辑数据项可有副本（读任一副本、写需锁全部副本）。**全文不涉及范围/区间/谓词/幻读锁对象**（`predicate` 一词仅见于参考文献条目 Eswaran 1976；该文在正文仅作为 2PL 正确性的引文 `[ESWA76]` 出现，PAGE 10；`phantom` 只出现在 PAGE 14 的 "phantom deadlock"，指死锁检测里的假环，与本综述的幻读无关，需避免误读）。
- **冲突判定**：形式化定义（PAGE 10）：rw 同步下两锁冲突 iff (a) 同一 data item 且 (b) 一个 readlock 一个 writelock；ww 同步下 iff 同一 data item 且都是 writelock。锁归属两规则：(1) 不同事务不能同时持有冲突锁；(2) 「once a transaction surrenders ownership of a lock, it may never obtain additional locks」。
- **关键机制**：2PL 的 growing / shrinking 阶段、**locked point**（「the point at the end of the growing phase, when a transaction owns all the locks it ever will own」）、predeclaration；「The serialization order attained by 2PL is determined by the order in which transactions obtain locks」；Basic 2PL 的 2PL scheduler / wait queue FIFO / dm-read / dm-write / prewrite；其余为 Primary Copy、Voting、Centralized 2PL 与 timestamp ordering、死锁检测、两阶段提交的组合。
- **正确性**：「Two-phase locking is a correct synchronization technique」，即得到无环优先图 / 冲突可串行化；locked points 决定串行化顺序（PAGE 10）。这是 RdmaRangeLock「严格 2PL」设定的理论基座。
- **代价**：论文自述「Issues of performance are given only secondary treatment」，结论（PAGE 36 附近）把吞吐量/响应时间与四类成本（站点间通信、本地处理、事务重启、事务阻塞）列为 open issue。
- **与 RdmaRangeLock 的关系**：**不是范围锁文献，而是 `detect_conflict(other)` + 严格 2PL 的形式化基座**。关键可迁移点：42 把「冲突」抽象成「同一 data item + 锁模式不兼容」；RdmaRangeLock 的所有版本都是把 `data item` 从「单点」换成「`key_range` 区间」、把「相等」换成「区间重叠」、把兼容矩阵从 S/X 扩成多级/意向锁，其余 2PL 论证不变。可用于综述「正确性模型」一节的统一表述。
- **可复现性**：无需复现（理论基座）。若要补一个「2PL 变体」自变量，可做 Basic 2PL vs. predeclaration（预先声明全部锁），但项目已固定严格 2PL + `unlock_all()`，无必要。
- **关键原文证据**（ACM Computing Surveys）：PAGE 1「**one approach, called two-phase locking, has been accepted as a standard solution**」「We describe 48 principal methods」；PAGE 10「**Two-phase locking (2PL) synchronizes reads and writes by explicitly detecting and preventing conflicts between concurrent operations ... (1) different transactions cannot simultaneously own conflicting locks; and (2) once a transaction surrenders ownership of a lock, it may never obtain additional locks.**」；PAGE 10「for rw synchronization two locks conflict if (a) both are locks on the same data item, and (b) one is a readlock and the other is a writelock」；PAGE 10「**the locked points of E determine a serialization order for E.**」

---

### 43 CMU 15-445/645 (Spring 2025) · Two-Phase Locking 课程幻灯片（2025, Prof. Jignesh Patel, 137 页幻灯片网页打印快照）

> 幻灯片逐帧展开，同一标题会重复多页（2PL Violation / Cascading Aborts / 2PL Example 等各占多帧）。本条依据 PAGE 1–35、95–105、132–137 及 Grep 全篇关键词。

- **一句话问题**：如何在不预知整个调度前提下保证冲突可串行化，以及为什么「一个对象一把锁」在十亿元组上不可行。
- **锁对象 / 粒度**：Database → Table → Page → Tuple（另画 Attribute）的**数据库锁层级**；粒度可由 DBMS 自选（「Attribute? Tuple? Page? Table?」）。锁模式 S / X / IS / IX / SIX。
- **冲突判定**：S/X 基础矩阵（PAGE 15）；IS/IX/S/SIX/X 五级兼容矩阵（PAGE 102）；层级协议（PAGE 103）：「To get S or IS lock on a node, the txn must hold at least IS on parent node. To get X, IX, or SIX on a node, must hold at least IX on parent node」。
- **关键机制**：**LOCKS VS. LATCHES 对照表**（PAGE 6–7，注明 `Source: Goetz Graefe`）——Locks 分离事务、保护数据库内容、贯穿整个事务、模式 S/X/Update/Intention、死锁靠 waits-for/timeout/aborts 检测、存于 Lock Manager；Latches 分离 worker、保护内存数据结构、仅临界区、模式 Read/Write、靠编码纪律避免死锁。**2PL**（PAGE 25–28）Growing / Shrinking，「The txn is not allowed to acquire/upgrade locks after the growing phase finishes」，违反示例标注 `2PL Violation!`；**2PL 只保证冲突可串行化但有 cascading aborts**（PAGE 34），故需 **Strong Strict 2PL (Rigorous 2PL)**；**LOCK GRANULARITIES**（PAGE 95）；**INTENTION LOCKS**（PAGE 100–101）；**LOCK ESCALATION**（PAGE 132）；SELECT...FOR UPDATE / SKIP LOCKED（PAGE 134–135）。
- **正确性**：「2PL on its own is sufficient to guarantee conflict serializability because it generates schedules whose precedence graph is acyclic. But it is subject to cascading aborts.」（PAGE 34）；SS2PL 解决 cascading aborts。
- **代价**：**「All these examples have a one-to-one mapping from database objects to locks. If a txn wants to update one billion tuples, then it must acquire one billion locks. Acquiring locks is a more expensive operation than acquiring a latch even if that lock is available.」**（PAGE 94）；权衡为「Fewer Locks, Larger Granularity vs. More Locks, Smaller Granularity」（PAGE 95）；LOCK ESCALATION「reduces the number of requests that the lock manager must process」（PAGE 132）。
- **与 RdmaRangeLock 的关系**：**v6 与动态升级/降级的教学级依据**。PAGE 94–95 那句「one-to-one mapping → 十亿把锁」正是 v6 用固定桶宽把「每键一把锁」压缩成「每桶一把锁」的动机；PAGE 96–103 的意向锁层级是 v6 层级桶锁的通用协议模板（桶 = 层级中的一层）；PAGE 132 的 escalation 是 v6 缺失的「运行时自适应」维度。与 12 号专利同源（Locks vs. Latches 表注明 Source: Goetz Graefe）。
- **可复现性**：v6 已实现（b16/b64/b256 即三个粒度）；「lock escalation 阈值」可作为单变量补一个版本：用 `get_lock_count()` 达到阈值时把细粒度锁合并为粗粒度锁，观察锁数量与假冲突。只需新增阈值参数与合并逻辑，`lock/unlock_all` 接口不变。
- **关键原文证据**（CMU 课程幻灯片网页打印快照）：PAGE 94「**All these examples have a one-to-one mapping from database objects to locks. If a txn wants to update one billion tuples, then it must acquire one billion locks.**」；PAGE 95「When a txn wants to acquire a "lock", the DBMS can decide the granularity (i.e., scope) of that lock ... **Fewer Locks, Larger Granularity vs. More Locks, Smaller Granularity.**」；PAGE 100「An intention lock allows a higher-level node to be locked in shared or exclusive mode without having to check all descendent nodes.」；PAGE 132「**The DBMS can automatically switch to coarser-grained locks when a txn acquires too many low-level locks. This reduces the number of requests that the lock manager must process.**」；PAGE 25「Phase #1: Growing ... Phase #2: Shrinking → The txn is allowed to only release/downgrade locks that it previously acquired. It cannot acquire new locks.」；PAGE 34「2PL on its own is sufficient to guarantee conflict serializability ... But it is subject to cascading aborts.」

---

### 44 Comer · The Ubiquitous B-Tree（1979, ACM Computing Surveys, Vol. 11, No. 2, June 1979, pp. 121–137）

- **一句话问题**：综述 B-tree 及其变体为何成为事实标准，并首次系统交代 B+树在多用户环境下的**结构并发**问题。
- **锁对象 / 粒度**：B+树的**节点/页**（结构侧），非事务可见数据；B+树结构本身：所有 key 在叶层，上层仅作 index/roadmap，叶节点左右相连成 **sequence set**。
- **冲突判定**：结构冲突——「One process may read a node and follow one of the links while another process is changing it」；find 自顶向下、insert/delete 自底向上，方向相反使并发更复杂。
- **关键机制**：B-tree 基础（PAGE 3–7，分裂时「the remaining value is promoted to the parent node where it serves as a **separator**」，PAGE 6）；**B+-tree**（PAGE 9–10）「all keys reside in the leaves ... leaf nodes are usually linked together left-to-right ... The linked list of leaves is referred to as the **sequence set**. Sequence set links allow easy sequential processing」；删除时「the ability to leave non-key values in the index part as **separators** simplifies processing」；叶分裂「promotes a **copy** of the key, retaining the actual key in the right leaf」；**Prefix B+-tree**（Bayer & Unterauer 1977, PAGE 11）：「The separator value in the index between the keys "computer," "electronic" need not be either of them; **any string between suffices** ... The simple technique of choosing the **shortest unique prefix** of the key to serve as a separator works well」；**4. B-TREES IN A MULTIUSER ENVIRONMENT**（PAGE 13）：Bayer & Schkolnick 的锁协议——「a find locks, or holds, a node once it has been read ... As the search progresses to the next depth, the find processor releases its lock on the ancestor ... **Thus, readers lock at most two nodes at any time**」（latch coupling 原型）；更新进程在路径上留 **reservation**，必要时转成 absolute lock，自顶向下转换；Guibas & Sedgewick 的 top-down splitting「eliminates the need for all but the most simple protocols ... **only one pair of nodes will ever be locked at a given time**」。
- **正确性**：只保证树结构（find 能到正确叶、SMO 不破坏树），**不涉及幻读/谓词/事务隔离**——这是「结构并发」与「事务级范围锁」的分界线。
- **代价**：B+树把 `next` 操作降到最多 1 次访问（PAGE 10）；latch coupling 任意时刻最多锁两个节点；Prefix B+-tree 用短 separator 提高扇出、降低树高；代价是 reservation 机制与「reserving an entire path」会阻塞其他更新。
- **与 RdmaRangeLock 的关系**：**时间线起点与 v1/v2 的结构依据**。①「sequence set / 叶链」是 range scan 的物理基础，也是 v5 next-key（后继记录）与 `exact-range` 的物理载体；②「separator 可以是任意介于两键之间的值 / 最短唯一前缀」是 12 号 **separator-key 层级锁** 与 **key-prefix 锁** 的结构前提——Comer 用它省空间，Graefe 用它做锁名，两者是同一结构事实的两种利用；③Bayer & Schkolnick 的「最多锁两个节点」是 v2 latch coupling 的直接来源；④全文无幻读讨论，反证「结构并发不解决洞」这一因果链起点。
- **可复现性**：v1/v2 已实现；「Prefix B+-tree 的短 separator」可作为 separator 层级锁版本的一个自变量（separator 取真实键副本 vs. 取最短前缀），需 B+树内部节点暴露 separator 并可配置其生成方式。
- **关键原文证据**（ACM Computing Surveys）：PAGE 9「**In a B+-tree, all keys reside in the leaves. The upper levels ... consist only of an index, a roadmap** ... leaf nodes are usually linked together left-to-right ... **The linked list of leaves is referred to as the sequence set.**」；PAGE 10「the ability to leave non-key values in the index part as **separators** simplifies processing」「When a leaf splits in two, ... the algorithm promotes a **copy** of the key, retaining the actual key in the right leaf.」；PAGE 11「**The separator value in the index between the keys "computer," "electronic" need not be either of them; any string between suffices ... The simple technique of choosing the shortest unique prefix of the key to serve as a separator works well.**」；PAGE 13「**a find locks, or holds, a node once it has been read ... As the search progresses to the next depth, the find processor releases its lock on the ancestor ... Thus, readers lock at most two nodes at any time**」；PAGE 13「using the top-down splitting suggested in GUIB78 eliminates the need for all but the most simple protocols ... only one pair of nodes will ever be locked at a given time.」；PAGE 1「B-trees have become, de facto, a standard for file organization.」

---

## 本组发展史时间线

> 主线：**B+树结构并发 → 索引项区间锁（防洞） → 层级/前缀粒度（省锁） → 现代 precision locking 复活**。

| 年份 | 事件（本组文献 / 被引文献） | 因果作用 |
|---|---|---|
| 1972 | Bayer & McCreight 提出 B-tree（44 参考文献 `[BAYE72]`） | 提供「自顶向下查找 / 自底向上更新」的多路平衡树，埋下结构并发难题 |
| 1976 | Eswaran, Gray, Lorie, Traiger「predicate locks」（31 PAGE 3、29 `[9]`、42 `[ESWA76]`） | **逻辑锁**首次提出：事务应锁「逻辑子集」而非物理项；同时被 42 引为 2PL 正确性来源。一般谓词冲突判定后来被证明 NP-hard（29 PAGE 9），长期无法实用 |
| 1977 | Bayer & Schkolnick「Concurrency of operations on B-trees」；Bayer & Unterauer「Prefix B-trees」（44 `[BAYE77a]` / `[BAYE77]`） | 前者给出 latch coupling 原型（44 PAGE 13），后者给出「separator 可为任意介于两键之间的最短前缀」（44 PAGE 11）——**结构事实后来被 Graefe 用作锁名** |
| 1979 | **44 Comer《The Ubiquitous B-Tree》** | 固化 B+树（叶 sequence set + 上层 separator roadmap）与「多用户环境下的结构并发」。综述明确：结构锁只保护树，不保护洞、不防幻读 |
| 1981 | Lehman & Yao B-link tree（05 PAGE 1–3）；Jordan/Banerjee/Batman「Precision locks」（31 PAGE 2、29 `[12]`）；**42 Bernstein & Goodman** | B-link 用 high key + right-link 做到读完全不加锁；**Precision Locks 首次把 1976 的谓词限制为可计算形式**（题录称「PL usually is cheaper to execute」）；42 把 2PL 形式化为「同一 data item + 模式冲突 + growing/shrinking + locked point」，成为此后所有版本的正确性模板 |
| 1990–1992 | Mohan ARIES/KVL、ARIES/IM（29 `[24]`、31 参考文献） | **next-key / key-value locking 工程化**；05 PAGE 4 称「This is what everybody does for B-trees」——索引项区间锁成为事实标准 |
| 1995–1997 | Kornacker 等 GiST 并发 / 幻读保护（05 PAGE 4–6） | 提出「**把谓词 attach 到被搜索的索引节点**、叶子 overlock 作物理替身」，并总结「树层级 physio-logical hierarchy 可代替 next-key 防幻读」——**层级范围锁与谓词层级化的思想原点** |
| 2000 | **31 O'Neil/Fekete/Liarokapis PAX**（非全文题录） | KVL/IM（索引区间锁）+ 内存中断言谓词（PA / precision locking）的**混合**方案，谱系上连接 1976/1981 与 2024 |
| 2006 → 2011 | **12 Graefe 层级锁专利**（优先权 2006-10-06；US 2011/0208704 A1 公开 2011-08-25） | 针对「表→索引→叶页/单键」中间缺级，一次性给出三件套：**新锁模式 SQ/QS/XQ/QX/SX/XS**（把 key 与 gap 合并进一条锁表条目）、**separator-key 层级范围锁**（逻辑命名 `(index id, separator, level)`、分裂时复制锁、fence key、零额外 I/O）、**key-prefix 锁**；外加 **lock escalation / de-escalation / granularity changes** 动态粒度 |
| 2014 | **24 / 28 MySQL 5.7.5 + WL#6609**（InnoDB GIS / R-tree） | 工业界因「多维无全序、无法定义 next key」而**复活谓词锁（MBR predicate lock）**，并提出「把谓词 attach 到 GIS 索引节点以改善粒度」；`WL#7928` GeoHash 给出 2D→1D 回到 B+树的桥 |
| 2024 | **29 Griffin**（arXiv 2407.13294）、**06 Griffin-PPL**（CANDARW，非全文题录） | 29 用 **precision locking（Lu/Lp、谓词限定为 single key range）**避免 rescanning 的额外 B+树遍历，吞吐 3.1x/5.4x；06 指出 update-heavy 下集中式锁链表 + 单 sync manager 成为瓶颈，用去中心化锁表 + 并行释放做到 6.5x。**这是「现代 precision locking 复活」的完整闭环**：1976 因 NP-hard 被弃 → 1981 限定谓词 → 2024 在内存/混合索引上重新可用 |
| 2025 | **43 CMU 15-445/645 2PL slides** | 教学固化：2PL/SS2PL、多粒度层级、意向锁、LOCK ESCALATION；并明说「one-to-one mapping → 十亿把锁」是粗粒度的根本动机（`Source: Goetz Graefe`） |
| 2026-11-07 | **25 / 26 / 27 / 28 官方文档快照** | 提供可核对的工业触发条件口径：REPEATABLE READ + 非唯一/范围扫描 → next-key；唯一唯一查找 → 纯 record lock；READ COMMITTED → 关 gap；SPATIAL → MBR predicate lock |

**因果链读法**：
1. **结构并发不防幻读**：44/05 的 latch / B-link / latch coupling 只保证「树能找到数据」；05 PAGE 4 点破「2PL locks the data, but not the 'holes'」，于是必须把锁对象从「记录」扩展到「记录之间的区间」。
2. **索引项区间锁（v4/v5）**：以 ARIES/KVL → MySQL next-key 为代表，用「后继记录」当逻辑区间的物理替身；代价是每记录两把锁、假冲突、依赖全序（25/26/27）。
3. **层级/前缀粒度（v6 及 Graefe 三件套）**：为省锁，引入中间粒度。通用形式是表→页→键的意向锁层级（43、25 PAGE 2）；Graefe 把它「B+树化」为 separator-key 层级与 key-prefix 层级，并加上动态升级/降级（12）。**v6 固定桶宽 b16/b64/b256 是这条线里最简化的静态版本**：桶边界不随树变、不做 SMO 锁迁移、不做运行时调整。
4. **谓词锁复活**：一般谓词因冲突判定 NP-hard 长期被弃；MySQL 在无全序的 R-tree 上被迫用 specialize 的 MBR 谓词锁（24/28/25 PAGE 7）；Griffin 用 precision locking 把谓词限制为 single key range、操作限制为 Insert/Delete/Scan，在现代索引上「复活」且不需额外 B+树遍历（29）；06 补上锁表可扩展性。这条线与 `predicate` / `exact-range` / `rdma-predicate` 直接对应。

---

## 本组建议的可复现类别

> 每类均为「可在现有单棵 B+树 + `lock(mode, key_range)` / `unlock_all()` / `get_lock_count()` / `detect_conflict(other)` + 严格 2PL + 同一 workload 下做成一个版本，并用单一自变量对比」的粒度。按用户口径，**gap 与 next-key 合并为 `C3` 一类**。

### [REPRO] C1 · 结构并发（latch / latch coupling / B-link）
- **包含命名方案**：v1 单线程 B+树（无并发）、v2 latch coupling、B-link（high key + right-link）、top-down splitting。
- **共享不变式**：锁的是树节点/页（物理结构），短临界区释放，不持有到事务边界；`lock` 与 `latch` 分离。
- **可测自变量**：是否用 right-link、latch 持有长度（1 个节点 vs. 2 个节点 coupling）、SMO 是否自顶向下。
- **预期差异**：只影响结构并发吞吐，**不影响幻读**（所有版本在事务级都不防幻读）；作为 v1–v6 的基线段。
- **依据**：44 PAGE 13、05 PAGE 1–3、43 PAGE 6–7。

### [REPRO] C2 · 记录级 2PL（record-2PL，无洞）
- **包含命名方案**：v3 record-2PL、MySQL「唯一索引 + 唯一搜索条件」的纯 index-record lock、42 的 Basic 2PL 抽象。
- **共享不变式**：锁对象 = 单个已存在 key；冲突 = 同一 data item + S/X 不兼容（42 PAGE 10）；严格 2PL，`unlock_all()` 在事务末释放（26 PAGE 6）。
- **可测自变量**：锁表数据结构（hash vs. 链表）、是否 predeclaration。
- **预期差异**：点操作正确，但范围操作**必然出现幻读**（05 PAGE 4、27 PAGE 1）；锁数量 = 命中记录数，无假冲突。
- **依据**：42 PAGE 10、25 PAGE 3、26 PAGE 2、43 PAGE 34。

### [REPRO] C3 · gap / next-key 同一类（索引项区间锁）
- **包含命名方案**：v4 gap lock、v5 next-key lock（= record lock + 前驱 gap lock）、MySQL Gap/Next-Key/Insert-Intention/Supremum、Graefe 的 `QS/QX`（单独锁 gap）、Graefe `SQ/XQ/SX/XS`（合并命名）。
- **共享不变式**：锁对象 = 相邻索引项之间的**空隙**（以及末端 supremum gap），是「逻辑区间的物理替身」；插入必须越过 gap 锁；gap 锁「purely inhibitive」、S/X gap 互不冲突（25 PAGE 4）；next-key 区间形如 `(prev, key]`。
- **可测自变量**（单变量）：①是否锁 gap（v3 → v4）；②是否把 record 与 gap 合并命名（v4 → v5）；③是否锁 supremum 末端 gap；④gap 锁的兼容语义（互斥 vs. 纯抑制）；⑤唯一索引唯一查找时是否跳过 gap。
- **预期差异**：v3→v4 消除幻读但引入假冲突；v4→v5 只是命名合并，锁数量近似不变（每记录约两把 vs. 一把组合锁），冲突判定开销下降；关 gap（READ COMMITTED）幻读回归；末端 gap 决定上界外插入是否被挡。
- **依据**：25 PAGE 3–5、27 PAGE 1–2、26 PAGE 1–2、05 PAGE 4、12 `[0058]`。

### [REPRO] C4 · 精确区间锁 exact-range
- **包含命名方案**：`exact-range`（精确 `[lo,hi)`）、26 PAGE 1 的反面「按扫描足迹加锁」、05 PAGE 4 的 predicate（单区间退化形式）。
- **共享不变式**：锁对象 = 用户给出的精确 `[lo,hi)`，与 B+树遍历足迹无关；冲突 = 两区间相交 + 模式不兼容；严格 2PL。
- **可测自变量**：锁区间是「精确结果区间」还是「扫描足迹范围」；区间端点开闭（`[lo,hi)` vs `(lo,hi]`）。
- **预期差异**：精确区间锁数量恒为 1（每次范围操作一把），假冲突只来自区间重叠；扫描足迹锁数量随记录数增长（26 PAGE 1），假冲突更大但更贴近工业实现。
- **依据**：26 PAGE 1「InnoDB does not remember the exact WHERE condition, but only knows which index ranges were scanned」；27 PAGE 1 的 `(90,102)` 洞例子。

### [REPRO] C5 · 谓词 / 精度锁（predicate / precision / MBR）— **重点评估 ④**
- **包含命名方案**：`predicate`（一次范围操作一把整段区间 S 锁）、Griffin **Lu/Lp precision locking**（29）、MySQL **MBR predicate lock**（24/28/25 PAGE 7）、`rdma-predicate`、Eswaran 1976 predicate lock（31/29 引用）、PAX 的 PA（31，非全文，仅谱系）、05 PAGE 5 的「谓词 attach 到节点 / 叶子 overlock」。
- **共享不变式**：锁对象是**逻辑谓词**（本项目里即单个 key range，或一维退化 MBR），不是索引结构上的物理项；读侧登记谓词（Lp）、写侧登记被改键（Lu）；冲突 = 谓词与操作相交（Griffin 双向检查）；锁持有到事务终止（29 PAGE 3）；必须限制谓词形式，否则冲突判定 NP-hard（29 PAGE 9）。
- **可测自变量**（单变量）：①谓词形式（单区间 / 多区间 / 二维矩形）；②锁表组织（线性链表 vs. 哈希/区间树 vs. 分片去中心化 —— 对应 29 PAGE 8 与 06 的瓶颈）；③冲突检查方向（单向 vs. Lu↔Lp 双向）；④是否把谓词挂载到 B+树内部节点做剪枝（28 PAGE 1）。
- **预期差异**：`predicate` / Lu-Lp 恒为「一次范围一把锁」，锁数量最小、无需额外 B+树遍历（29 PAGE 1），但冲突检查是 O(|锁表|) 顺序比较，update-heavy 下锁链表膨胀会崩吞吐（06）；把谓词挂到节点/桶可把检查次数降到 O(路径长)，代价是假冲突上升（等价于 v6）。
- **能否在 B+树上做成可复现版本**：**能，且是本组最推荐补的一类**。
  - `predicate` 已实现；`precision locking` 只需把写侧 Insert/Delete 表示为退化 key（或 `[k,k]` 区间）放入 Lu、读侧 `[lo,hi)` 放入 Lp，`detect_conflict` 改为「区间重叠 + key∈range」，**接口完全不用改**；29 PAGE 10 明说 precision locking 与索引结构无关。
  - **MBR 需分情况**：真·二维矩形在现有单维 B+树 + `key_range` 下**不能直接做**，需新增二维键/矩形相交判定；但两条可复现路径成立：①**一维退化 MBR = `[lo,hi)` 区间**（即 `exact-range`/`predicate`），这是 WL#6609「specialize predicate」在 B+树上的自然形态；②按 24 PAGE 13 `WL#7928` GeoHash 把 2D 编码成 1D 键再套区间锁（需新增编码函数）。综述应明确区分「真 MBR 不可直接复现」与「一维退化/编码后可复现」，不要把 MySQL 的 R-tree MBR 锁说成能在单维 B+树上原样跑。
  - 需要新增：写侧单键 → `key_range` 的退化表示、锁表容器（可插拔）、可选的 `Commit/Abort` 状态标记与 GC 钩子（若模拟 29 的 sync manager）；**不需要**改 B+树结构。

### [REPRO] C6 · 静态固定桶宽层级锁（v6）
- **包含命名方案**：v6 b16 / b64 / b256；MySQL 表级意向锁层级（25 PAGE 2、43 PAGE 96）；05 PAGE 5「overlock: put True on all leaves you touch」的节点级粗锁。
- **共享不变式**：键空间被**静态等宽**切成桶，锁名 = 桶 id；`[lo,hi)` 映射到若干连续桶；桶边界与 B+树结构无关，SMO 不迁移锁；层级用 IS/IX 意向锁（43 PAGE 103）。
- **可测自变量**：**桶宽 b**（16/64/256，单一连续变量）；是否加意向锁层级。
- **预期差异**：b 越大 → `get_lock_count()` 越小、锁表开销越低，但跨桶边界的**假冲突**越大；偏斜数据下固定桶宽尤为不利。这是「Fewer Locks, Larger Granularity vs. More Locks, Smaller Granularity」（43 PAGE 95）的直接量化。
- **依据**：43 PAGE 94–95、25 PAGE 2、12 `[0002]`–`[0003]`。

### [REPRO] C7 · separator-key 层级范围锁 — **重点评估 ①**
- **包含命名方案**：Graefe `[0072]–[0084]` separator key range locking、44 PAGE 6/10/11 的 separator / Prefix B+-tree 最短 separator、B-link fence key（05 PAGE 1、12 `[0049]`）、05 PAGE 6 的 physio-logical hierarchy。
- **共享不变式**：锁名 = `(index id, separator key value, node level)`，**逻辑位置而非物理页 ID**；一个 separator 锁覆盖 `[该 separator, 下一个 separator)` 半开区间（12 PAGE 20 `[0073]/[0076]`）；最左区间与最高键由 **fence key** 定界；**SMO 不变式**：分裂时把该区间上的锁复制到新 separator，合并时校验合并锁集无冲突（12 `[0083]/[0084]`）；层级协议用 IS/IX。
- **可测自变量**（单变量）：**锁所在层级** leaf(level 0) / parent(level 1) / grandparent(level 2)，或等价地 B+树**扇出**（决定每层省锁倍数）；次级自变量：separator 取真实键副本 vs. 最短唯一前缀（44 PAGE 11）。
- **预期差异**：层级越高锁数量越少（12 `[0075]`「may save tens or hundreds of locks」）、假冲突越大；大范围顺序扫描时「parent and grandparent nodes enable deep read ahead ... does not incur any extra I/O」（12 `[0048]`），故额外遍历成本极低；与 v6 相比，边界随树动态、偏斜数据下假冲突更小，但需实现 SMO 锁复制。
- **能否在现有 B+树 + 固定接口下做成单变量版本**：**能**。需要新增：①B+树内部节点暴露 separator 键列表与 node level（v1 的树已有内部节点，只是未暴露）；②锁名从「桶 id」改为 `(level, separator)`；③`detect_conflict` 支持「不同 level 的锁通过意向锁/区间包含关系判冲突」；④插入触发分裂时实现锁复制（若 workload 只做静态树上的范围查询，可先固定树不做 SMO，把 SMO 作为后续自变量）。`lock/unlock_all/get_lock_count` 接口不变，`key_range` 仍需用于定位 `[lo,hi)` 落在哪些 separator 区间。
- **与 v6 的关系（综述必写）**：v6 是「固定桶宽、与树结构解耦、静态」；C7 是「边界由 B+树真实 separator 决定、随 SMO 演化、层级 = 树高、运行时可升级」。C7 把 v6 的单层桶推广为「与树同构的多层粗锁」，是 v6 的直接升级版（建议编号 v7）。

### [REPRO] C8 · key 前缀锁 — **重点评估 ②**
- **包含命名方案**：Graefe `key prefixes`（12 ABSTRACT / `[0005]` / FIG.12–14）、Comer Prefix B+-tree（44 PAGE 11）、12 FIG.13 的「JOHNSON 多个逐键锁 → 前缀 S 锁」、24 PAGE 13 GeoHash 的「前缀即空间分区」思想。
- **共享不变式**：锁名 = 键的**前缀串**（如 "JOHNSON"、"JOHNSON,A"）；一个前缀锁覆盖所有以该前缀开头的键；前缀越短粒度越粗、越省锁；多级前缀构成层级（12 FIG.14 的 `LASTNAME, FIRSTNAME, MI`）；同样遵循意向锁层级协议。
- **可测自变量**（单变量）：**前缀长度 / 前缀层级数**（1 级、2 级、3 级）；或「前缀 vs. 固定桶宽 vs. separator」三种切分方式对比。
- **预期差异**：字符串/复合键负载下，短前缀一把锁覆盖大量键，`get_lock_count()` 最小；但对均匀分布数值键，前缀切分粒度极不均匀（"1"、"10"、"100" 量级差异），假冲突可能远大于固定桶宽；对偏斜的字母型数据则明显优于桶宽。
- **能否在现有 B+树 + 固定接口下做成单变量版本**：**能，但需改 workload 的键类型**。需要新增：①键为字符串/复合键的 workload（现有数值键无法体现前缀）；②`key_range` 到「前缀集合」的映射函数（把 `[lo,hi)` 翻译成覆盖它的最短前缀集）；③锁名支持字符串前缀。B+树结构本身不用改（前缀是逻辑命名）。**注意**：12 号 txt 的 key-prefix 详细段落与权利要求在 PAGE 21–25 空白，未抽取，因此本类别的具体算法步骤只能依据 ABSTRACT、`[0005]` 与 FIG.12/13/14 复原到「前缀锁 + 前缀升级」这一层，不能声称更多细节来自该专利全文。
- **依据**：12 PAGE 1 ABSTRACT、PAGE 16 `[0005]`、PAGE 13–15 FIG.12/13/14；44 PAGE 11。

### [REPRO] C9 · 动态锁升级 / 降级 — **重点评估 ③**
- **包含命名方案**：Graefe `[0045]–[0047]` escalation / de-escalation / granularity changes（FIG.9/10/11）、43 PAGE 132 LOCK ESCALATION、12 `[0003]` 的「10,000 locks → index lock」困境、MySQL 隐式「无合适索引时全表锁」（26 PAGE 1）。
- **共享不变式**：事务运行时按「已持锁数量 / 争用」把多把细粒度锁合并为一把粗粒度锁（escalation），或反向拆细（de-escalation）；**不破坏严格 2PL**（升级算获取新锁，必须发生在 growing 阶段；降级/释放算 shrinking）；de-escalation 要求事务在私有内存保留恢复细粒度锁所需信息（12 `[0046]`）；层级锁必须由意向锁保护（43 PAGE 103）。
- **可测自变量**（单变量）：**升级阈值 T**（`get_lock_count() ≥ T` 时触发）；次级：升级目标层级（桶 / 父节点 / 全表）、是否允许 de-escalation。
- **预期差异**：T 小 → 锁数量低、锁表开销低，但假冲突与阻塞上升；T 大（或禁用升级）→ 并发高但锁数量线性增长（43 PAGE 94 的「十亿把锁」）；de-escalation 在争用上升时回收并发度，但需额外私有内存。12 `[0045]` 指出动机正是「inaccuracy of cardinality estimation」与「unpredictable concurrency contention」——可用偏斜/长扫描 workload 放大该差异。
- **能否在现有 B+树 + 固定接口下做成单变量版本**：**能，且是 v6 最直接的补强**。需要新增：①由 `get_lock_count()` 驱动的阈值判定与「合并细粒度锁为粗粒度锁」的逻辑；②事务私有内存缓存（记录被合并的细粒度键，供 de-escalation 用）；③粗/细粒度锁共存的层级兼容判定（`detect_conflict` 扩展为含意向锁）。**接口完全不用改**（这正是 `get_lock_count()` 存在的意义）。注意：项目已固定「严格 2PL + `unlock_all()` 事务末释放」，因此**升级必须在 growing 阶段、降级只能在 shrinking 阶段**，不能做成运行期随意升降级，否则违反 2PL（43 PAGE 25–28、42 PAGE 10）——这是复现时最容易踩的正确性约束。
- **依据**：12 PAGE 18 `[0045]–[0047]`、PAGE 10–12 FIG.9/10/11；43 PAGE 132；25 PAGE 2、26 PAGE 6。

### [REPRO] C10 · 新锁模式（key 分量 / gap 分量合并）
- **包含命名方案**：Graefe `SQ / QS / XQ / QX / SX / XS`（12 `[0056]–[0063]`、FIG.5）。
- **共享不变式**：key 与 gap 是**同一条锁表条目（按 key value 哈希）的两个分量**，`Q` = 该分量不锁；组合本身隐含 IS/IX 意向锁；兼容值由分量构造直接推导（如 `SQ` 与 `QX` 兼容）。
- **可测自变量**（单变量）：`mode` 取值集合大小（S/X 两种 vs. 6 种）；是否把 record 与 gap 合并进一条锁表条目。
- **预期差异**：合并后锁表条目数与 lock manager 调用次数减半（12 `[0056]`「only one resource and one invocation of the lock manager at the expense of additional lock modes」），`get_lock_count()` 下降、兼容矩阵变大；`QX` 插入只锁 gap 不锁既有键，可让其他事务并发读/改该既有键（12 `[0070]`），并发度高于 v5。
- **能否复现**：能，但需把 `mode` 从 S/X 扩成 6 种并改 `detect_conflict(other)` 的兼容矩阵，是接口层的小改动；可作为 C3 的一个「命名/并发度」子版本。
- **依据**：12 PAGE 5 FIG.5、PAGE 18–19 `[0056]–[0063]`、PAGE 20 `[0070]`。

### 四类重点评估结论汇总

| 重点 | 能否在现有 B+树 + 固定接口下做成单变量版本 | 需要新增什么 | 主要风险 |
|---|---|---|---|
| ① separator-key 层级范围锁 | **能**（建议 v7）。自定变量 = 锁层级 / 扇出 | 内部节点暴露 separator + level；锁名改 `(level, separator)`；跨 level 的意向锁冲突判定；SMO 时锁复制（可先固定树） | 12 号专利正文止于 `[0085]`，`[0086]+` 未抽取；SMO 不变式须自行实现并验证 |
| ② key 前缀锁 | **能，但需改 workload 键类型**（字符串/复合键） | 前缀映射函数、`[lo,hi)` → 前缀集合、字符串锁名；数值键 workload 不适用 | 12 号 key-prefix 详细段落/权利要求在空白页，细节不可据 txt 编造；分布不均时假冲突大 |
| ③ 动态锁升级/降级 | **能**，且是 v6 最自然补强；自变量 = 阈值 T | `get_lock_count()` 驱动阈值、锁合并逻辑、事务私有细粒度缓存、层级兼容判定 | **必须遵守严格 2PL**：升级只在 growing、降级只在 shrinking，否则破坏正确性（42 PAGE 10 / 43 PAGE 25–28） |
| ④ 谓词 / precision / MBR | **能**（`predicate` 已实现；precision 只需退化单键 + Lu/Lp 双向检查，接口不改）；**真二维 MBR 不能直接做**，一维退化或 GeoHash 编码后可做 | 写侧单键→`key_range` 退化表示；可插拔锁表容器；可选状态标记/GC；二维需新增矩形类型与相交判定 | 冲突检查 O(\|锁表\|)，update-heavy 下锁链表膨胀（29 PAGE 8 / 06）；一般谓词冲突判定 NP-hard，必须保持「谓词 = 单区间」限制（29 PAGE 9） |

**一条贯穿性建议**：把 `C6`（v6 固定桶宽）、`C7`（separator 层级）、`C8`（前缀）、`C9`（动态升级）放在同一张「粒度」实验里，自变量统一为**「每个锁覆盖的键区间宽度」**——只是「宽度由谁决定」不同：固定常数（v6）/ 树内部节点 separator（C7）/ 键内容前缀（C8）/ 运行时阈值（C9）。这样四类能用同一 workload、同一 `get_lock_count()` 与假冲突率指标直接单变量对比，正好回应用户「可以作为一个实现版本、能用单变量实验对比」的类别口径；而 `C5` 谓词锁则作为「锁对象从物理/结构项换成逻辑谓词」的正交维度，与粒度维度构成综述的两条主轴。
