# G4 组阅读笔记：结构并发 → 现代硬件早期锁释放 → 分离式内存锁服务

> 负责编号：15、17、18、19、20、35、38、40（共 8 篇）
> 全文来源：`D:\codex\zongshu\.extract\*.txt`，页码均为抽取文本中的 `===== PAGE n =====` 标记页。
> 已用 Grep 核对的关键词：`lock table`、`HOCL`、`on-chip`、`handover`、`CAS`、`FAA`、`version`、`latch`、`B-link`、`high key`、`right link`、`link pointer`、`ghost`、`early lock release`、`delta index`、`fence key`、`command combination`、`queue-notify`、`phantom`。
> 说明：关键词表里的 `segment tree` 在本组 8 篇全文中**均未出现**（应为其他组的材料），本组不作臆造；`delta index` 出现在 35 号 XIndex；`ghost` 出现在 17（ghost records）与 18（ghost nodes 脚注）。

---

## 0. 本组导读：两层必须分开（综述目前缺的那一维）

本组 8 篇恰好横跨用户要求区分的两个层次，建议综述据此**新增「锁服务实现层」一章**：

- **锁语义层（锁什么、怎么判冲突）**：范围/间隙锁保护「不存在的键」（17 号 §2）、gap 与 next-key 是同一类（17 号明确写出 next-key 与 prior-key 只是承载方向不同）、层级意图锁（17 号 §3）、版本化无锁范围查询（15 号）。这一层决定 `lock(mode, key_range)` 里 `key_range` 怎么解释、`detect_conflict()` 返回什么，对应 RdmaRangeLock 的 v3/v4/v5/v6/predicate。
- **锁服务实现层（锁表放哪、一次 `lock()` 展开成几个 verb / 几个往返）**：38 号 Sherman、20 号 LOTUS、40 号 DecLock 三篇**几乎没有改动锁语义**（都是「节点/记录级互斥锁」），全部优化的是 `lock()` 展开后的 verb 序列与锁表位置。这一层正是跨主机实测「每把锁约 5 个单边 RDMA 操作（抢门 CAS、读计数、读槽、写槽、释放门）、实测均值约 5.6 个往返/锁」的直接理论来源，也是本组最该补进综述的一维。

**关键约束呼应**：项目规定 `src/index` 只允许 latch、`src/lock` 只允许 lock，两者不得混用。本组给出三条独立佐证：

- 19 号 PAGE 1 脚注：「we always use the term "lock" instead of "latch" since we focus on low-level data structure synchronization, not high-level concurrency control」——即结构层同步（latch）与事务层并发控制（lock）术语必须分家。
- 35 号 PAGE 9：「Since XIndex does not support transactions ... we eliminate the conflicts」——学习索引只做单记录版本/锁，不做事务级范围锁。
- 38 号 PAGE 8：「like FG, Sherman does not guarantee that a range query is atomic with concurrent write operations. If upper applications, e.g., transaction engine, need snapshot semantics for range queries, they must use other mechanisms to avoid phantom problem」——**索引层 latch/version 明确不负责防幻读，必须由 `src/lock` 的范围锁补上**。这是 RdmaRangeLock 项目分工合法性的最硬证据。

**按「可做单变量对比的实现版本」粒度的类别表**（遵循用户口径：gap-lock 与 next-key-lock 算同一类）：

| 类别 | 层次 | 本组论文 | 对应 RdmaRangeLock |
|---|---|---|---|
| C1 结构并发 latch：B-link right-link + high key + 有序 latch | src/index latch | 18 | v2 latch coupling |
| C2 乐观版本 latch：OLC / ROWEX（64-bit version） | src/index latch | 19 | v2 变体 |
| C3 单记录版本锁 + delta index + 两阶段压缩 | src/index + 单记录锁 | 35 | v1 / v3 变体 |
| C4 无锁版本化范围扫描（vValue + 全局 Timestamp） | 无锁语义替代 | 15 | predicate / exact-range 对照组 |
| **C5 gap-lock 与 next-key-lock = 同一类**（相邻键承载间隙，仅方向不同） | **锁语义** | 17 §2（prior-key vs next-key） | **v4 = v5 同类别** |
| C6 层级/意图锁（intent / 桶锁） | 锁语义 + 实现 | 17 §3 LIL、38 HOCL 层级、40 CN 级层级 | v6 层级桶锁 |
| C7 锁服务实现：远端自旋锁表（CAS 抢门 + FAA/WRITE 释放，4 RTT 基线） | 锁服务实现 | 38 对 FG 的复现、20 对 Motor/FORD 的复现 | 「5 / 5.6 往返每锁」基线 |
| C8 锁服务实现：片上锁表 + 本地委托 + handover + 命令合并 | 锁服务实现 | 38 | 5.6 → 3 / 2 |
| C9 锁服务实现：队列通知（FAA 入队 + CN↔CN 通知）替代 CAS 自旋 | 锁服务实现 | 40 | 5.6 → ~1 |
| C10 锁服务实现：锁表整体解耦到 CN + 分片 + 批量 + lock-first | 锁服务实现 | 20 | 5.6 → 0（MN 侧） |
| C11 锁生命周期：严格 2PL vs 早期锁释放 ELR | 锁语义（持锁时长） | 17 §5 | `unlock_all()` 时机 |

> 关于 C5 的单变量说明：17 号 PAGE 2 明写「Mohan's and Lomet's designs employ 'next-key locking,' i.e., a gap between keys is locked using the next-higher key value, **this design uses 'prior-key locking' instead**」。二者冲突判定完全相同，单变量只是「间隙由右邻键还是左邻键承载」，因此按用户口径必须归为同一类、不能拆成两个实现版本；RdmaRangeLock 的 v4 gap 与 v5 next-key 也应合并为一个可对比版本。

---

## 逐篇笔记

### 18 B-link 树（1981, ACM TODS 6(4):650–670, Lehman & Yao）
- **一句话问题**：磁盘 B 树在并发插入/分裂时，查找进程会因「先拿到旧指针、再读到已被改动的节点」而错判（`PAGE 6` 的反例：search 15 与 insert 9 交错导致 `error: 15 not found!`），如何用最少的锁让结构修改与查找正确共存。
- **保护对象 / 粒度**：物理磁盘页（= 树节点）上的**排他 latch**；不锁键、不锁范围、不锁逻辑记录。查找路径完全不加锁。属于 `src/index` 的 latch 层，与事务锁语义无关。
- **冲突或一致性判定**：无读锁、无版本号；靠 `high key`（节点上界）+ `link pointer`（右链）判结构是否已被改动——`PAGE 8`：「If the search key exceeds the highest value in a node (as indicated by the high key), it indicates that the tree structure has been changed, and that the twin node should be accessed using the link pointer」。写-写冲突由页 latch 互斥；死锁靠良序（自底向上、同层自左向右，`PAGE 14` Lemma 1）。
- **关键机制（数据结构与步骤）**：每节点加一个 `link` 指针（`P_{2k+1}`，`PAGE 8`），指向同层右邻节点；分裂时先写新右半节点 `put(B,u)`、再原子地改写原页 `put(A,current)` 使其 link 指向 B（`PAGE 15` Lemma 2 证明两次 put 等价于一次结构变更），父节点指针稍后补齐；查找 `scannode` 把 link 当普通指针跟随，叶层持续右移直到 high key ≥ 查询键（`PAGE 10–11`）；插入用栈记住每层最右节点，回溯分裂，`move.right` 采用「先锁右、再放左」的左到右顺序。
- **正确性**：`PAGE 14` Theorem 1 无死锁（良序加锁）；`PAGE 15` Theorem 2 所有 put 保持树结构；`PAGE 16` Theorem 3（交互定理）证明插入不破坏其他进程正确性，依赖 `get`/`put` 的原子性与「分裂只向右移动条目」。已知不保证 livelock（`PAGE 18` 承认极慢处理器可能无限追 link）。**不涉及可串行化/幻读**：只保证物理结构一致。
- **代价**：`PAGE 19`：「at most a constant number of locks (three)」；查找零锁、零额外 I/O（仅分裂时多一次磁盘读）；无写放大、无假冲突（锁只落在被改节点）。代价是 high key/link 的少量存储与追随右链的偶发延迟。
- **与 RdmaRangeLock 的关系**：对应 **v2 latch coupling** 的原始形态、结构并发章。B-link 的 sibling/right link 是后面所有 RDMA 索引的地基——38 号 Sherman `PAGE 5` 明写「We record a sibling pointer for every leaf node and internal node as in the B-link tree」，38 号复现的 FG 也用 B-link。**不含范围锁语义**，正好说明「结构 latch 不能替代 `src/lock`」，为项目 `src/index`/`src/lock` 分离提供历史起点。
- **可复现性**：**能**做单变量消融且完全不动锁语义。改 `src/index`：给树节点加 `high_key` + `right_link` 字段，把 latch coupling 的「同时持父子锁」改为「右移重试 + 左到右加锁」，`src/lock` 一行不改。可扫描变量：节点容量 2k、分裂频率、并发查找/插入比（`PAGE 19` 建议用仿真量化）。
- **关键原文证据**：
  - `PAGE 1`：「A single additional "link" pointer in each node allows a process to easily recover from tree modifications ... the locking scheme is simpler (**no read-locks are used**) and only a (small) constant number of nodes are locked by any update process at any given time.」
  - `PAGE 11`：「Note also that this procedure **does no locking of any kind**. This contrasts with conventional database search algorithms ... in which all searches read-lock the nodes they examine.」
  - `PAGE 19`：「The solution given above for insertion uses **at most a constant number of locks (three)** for any process at any time.」
  - `PAGE 18` 脚注：「this statement ignores the problem of "**ghost**" nodes created by deletion」（ghost 概念的最早出处之一，17 号把它做成 ghost records）。

### 19 ART 实用同步：OLC 与 ROWEX（2016, DaMoN'16, Leis et al.）
- **一句话问题**：多核 + 全内存环境下细粒度锁本身成为可扩展性瓶颈（读锁也要写共享内存 → cache line 失效），如何在「难写的 lock-free」与「不伸缩的 lock coupling」之间找中间地带。
- **保护对象 / 粒度**：ART（Adaptive Radix Tree）**节点**；OLC 的乐观锁 = 一个 64-bit word（lock 位 + obsolete 位 + version 计数），ROWEX 的 per-node 写锁。仍是 `src/index` 的 latch 层。
- **冲突或一致性判定**：OLC 用 `readLockOrRestart` 取版本、`readUnlockOrRestart` 校验「锁仍空闲且版本未变」，不一致就从根重启；解引用指针前必须额外校验一次（`PAGE 3` line 11）。ROWEX 读者**完全不校验版本**，由写者用原子 store 保证读到的中间状态安全（新增 `level` 字段识别被截断的 path compression，`PAGE 4`）。二者都靠 epoch-based reclamation 延迟回收被删节点。
- **关键机制**：OLC = 传统 lock coupling（同时持 ≤2 锁）+ 版本读；`PAGE 7` 附录 A 给出原语：`upgradeToWriteLockOrRestart` 用 `node.version.CAS(version, setLockedBit(version))`，`writeUnlock` 用 `fetch_add(2)`，`writeUnlockObsolete` 用 `fetch_add(3)`，`awaitNodeUnlocked` 自旋等锁位。ROWEX 额外要求：Node4/Node16 不再保持键有序（追加写、删除置 null、惰性回收）以便并发；节点替换四步（锁节点+父 → 复制新节点 → 原子 store 改父指针 → 标记 obsolete 并解锁，`PAGE 4`）。
- **正确性**：保证多节点一致快照（linearizable 的读视图）、写者互斥；`PAGE 3` 明确必须防「无限循环 + 无效指针」，故乐观读到的指针解引用前须版本复核；病态重启用「重启次数上限后改拿写锁」兜底。ROWEX 正确性依赖 ART 不变式「无重平衡、每个键有确定物理位置」（`PAGE 4`）。**不涉及事务隔离/范围锁**。
- **代价**：`PAGE 4`：每节点额外空间 lock coupling 4B、OLC 8B、ROWEX 12B；`PAGE 5` CPU 统计：20 线程下 lock coupling 的 L1 miss 从 5.2 涨到 9.0、cycles 涨 6.6×，OLC 仅 348→418 cycles；OLC 单线程相对无同步约 65% 指令开销、20 线程降到 10%；代码行数 `PAGE 6`：lookup 41/44/34 行。假冲突低，但 OLC 读在冲突下会重启。
- **与 RdmaRangeLock 的关系**：对应 **v2 latch coupling 的现代变体**、结构并发章。`PAGE 1` 脚注是项目 `src/index` 只允许 latch 的直接术语依据；`PAGE 6`「OLFIT ... uses the B-link tree idea [11] to support concurrent structure modification operations」把 18 与 19 串成一条线（版本管节点内、right-link 管跨节点）；`PAGE 6` 对 Bw-Tree 的「delta records are pre-pended to a list ... atomically add new delta records」正是 35 号 delta index 的前身。ROWEX「写者承担一致性、读者零校验」与 38 号 two-level version 的「写者只回写 entry」思路同源。
- **可复现性**：**能**，纯 `src/index` 单变量消融：把 v2 的 spinlock latch 换成 64-bit version + CAS 升级（OLC），再进一步换成「读者不校验、写者原子 store + level 字段」（ROWEX）；`src/lock` 的 `lock()`/`detect_conflict()` 完全不改，锁语义零变化。采集指标：每 lookup cycles、L1/L3 miss、重启次数、每节点额外字节。
- **关键原文证据**：
  - `PAGE 1` 脚注：「we always use the term "**lock**" instead of "**latch**" since we focus on low-level data structure synchronization, not high-level concurrency control.」
  - `PAGE 2`：「Modifications are detected after the fact using **version counters**, and the operation is restarted ... we encode the lock and the version counter (and an obsolete flag) in a **single 64 bit word** that is updated atomically.」
  - `PAGE 2`：「concurrent locking of tree structures causes many unnecessary cache misses ... Threads, in effect, "fight" for exclusive ownership of the cache line holding the lock.」
  - `PAGE 6`：「Hand-over-hand locking, i.e., the idea underlying Optimistic Lock Coupling, was used to synchronize binary search trees」；「OLFIT does not keep track of versions across multiple nodes, but uses the **B-link tree idea**」。

### 35 XIndex：可扩展学习索引（2020, PPoPP'20, Tang et al.）
- **一句话问题**：学习索引（RMI/CDF 模型）不支持写，且对数据与查询分布敏感；如何在多核上支持并发写、范围扫描与运行时结构调整而不阻塞查询。
- **保护对象 / 粒度**：`group`（range partition）内的**单条记录**：`record_t` 内嵌 `lock:1, version:61`（`PAGE 4` Algorithm 1）；delta index 基础版用「单个全局读写锁」、优化版用「每节点 version + lock」（`PAGE 9`）。属于 `src/index` 层，**不做事务**。
- **冲突或一致性判定**：get 先快照 `rec.ver`，读完值后校验「`!rec.lock && rec.version == ver`」否则重试（`PAGE 7` Algorithm 5 `read_record`）；put/remove 先拿 per-record lock，`write` 后 `rec.version++`。写-写冲突由 per-record lock 串行化；与后台 compaction/structure update 的交叉用 **RCU barrier** 协调（`PAGE 7–8`），要求「no successful put will be lost」「no duplicate records」，并保持新鲜度序 `data_array ≽ buf ≽ tmp_buf`。
- **关键机制**：两层结构（root RMI 索引 group，group 内线性模型 + data_array + **delta index** 缓冲插入）；**Two-Phase Compaction** 分 merge phase（新数组放的是指向旧记录的引用 `is_ptr`，`buf_frozen` 阻止新 put 进 buf，`tmp_buf` 接住并发插入）与 copy phase（RCU barrier 后 `replace_pointer` 把引用原子换成真值）；`get_group` 还要检查 `next` 指针以找到尚未被 root 索引的新建 group；运行时按 error bound / delta size 阈值做 model split/merge、group split/merge、root update（`PAGE 8` Table 2）。
- **正确性**：`PAGE 8` 正确性条件「a get(k) must observe the latest committed put(k, v)」= linearizability，靠三条归纳不变式 I1/I2/I3 证明；依赖 RCU barrier 让旧 group 在 copy phase 前无人访问，以及 get/put 相同的查找顺序 `data_array → buf → tmp_buf`。`PAGE 9` 自承「Since XIndex does not support transactions」——**无隔离级别、无幻读保护**。
- **代价**：写放大来自 compaction 的数据拷贝（`PAGE 11`：128B value 的拷贝开销是 8B 的 13.5×）；`PAGE 9` 配置 e=32、s=256、m=4、1/12 线程作后台线程；长键（64B）最多降 50% 性能（`PAGE 12`）；delta index 的全局锁是同 group 并发写的争用点。
- **与 RdmaRangeLock 的关系**：对应 **v1 单线程 / v3 record-2PL 的结构侧对照**、结构并发章。`PAGE 3` 的「buffer all writes in a delta index, then periodically compact ... concurrent requests are blocked by the compaction」是 15 号 Kanva 攻击的靶子；`PAGE 3` Table 1 的 skewed 分布误差界（15.71/19.52）说明「模型/桶粒度」本身会制造类似**假冲突**的访问偏斜，可类比 RdmaRangeLock 的桶分片。**不含 `lock(mode, key_range)` 语义**，再次印证索引层不能替代锁层。
- **可复现性**：**能**，改 `src/index`：加 delta index + 两阶段压缩 + RCU barrier + per-record version；`src/lock` 不动。但若要与 RdmaRangeLock 对比谓词锁，需注明「XIndex 无事务层，范围扫描不防幻读」，即只能作为「结构层能做到哪一步」的上界，不能作为范围锁实现版本。
- **关键原文证据**：
  - `PAGE 2`：「XIndex **range-partitions** data into different groups, each attached with learned models for lookup and a **delta index** to handle inserts.」
  - `PAGE 3`：「an intuitive solution is to **buffer all writes in a delta index**, then periodically compact it ... **concurrent requests are blocked by the compaction**, which is time-consuming.」
  - `PAGE 4` Algorithm 1：`record_t` 含「`lock : 1, version : 61`」；「The `buf` is the **delta index**, which buffers all insertions.」
  - `PAGE 9`：「**Since XIndex does not support transactions**, in order to avoid the impact of transaction abortions due to conflicts, we eliminate the conflicts」；`PAGE 7`：「XIndex ensures readers can always fetch a consistent result ... with **locks and versions**」。
  - `PAGE 12`：「the overhead of data copying during compaction (128B's overhead is 13.5× larger than 8B's)」。

### 15 Kanva：无锁学习索引与线性一致范围查询（2023, arXiv:2308.11205, Bhardwaj et al.）
- **一句话问题**：已有的并发学习索引（XIndex/FINEdex）用细粒度锁 + barrier 做 retrain，且**范围查询不是线性一致的**；能否用纯 CAS 做到 lock-free 且范围查询 linearizable。
- **保护对象 / 粒度**：不保护「锁」，保护**键的版本链**：每个 key 关联一个 `vValue` 链表（value + 更新时间戳 `ts`），全局原子整数 `Timestamp`。粒度是「键/值对」而非区间锁。属于无锁语义（既非 latch 也非 lock）。
- **冲突或一致性判定**：`INSERT/DELETE/SEARCH` 全靠 CAS（`WRITE_VALUE` 反复 CAS 更新 `versions[ix]` 头指针；`HELP_MAKE_MODEL` 用 CAS 把 bin 换成 MNode；冻结 KNode 用 bit-stealing）；`DELETE` 不物理删除，只把值置 null。**范围查询的线性化点 = 原子读全局 `Timestamp`**（`PAGE 9`），`RANGE` 先 `READ_TIMESTAMP()` 把它 +1，然后 `SCAN` 只收集 `ts ≤ 起始 ts` 的版本（`PAGE 7` Algorithm 5，`READ_VALUE` 沿 `vnext` 走到第一个 `ver.ts ≤ ts`）。`PAGE 7`：「a range search is oblivious to an ongoing transformation of a Bin object, which is only replaced using an atomic CAS」。
- **关键机制**：浅层 RMI 模型 → 到达「non-blocking bins」；bin 生命周期 one-level bin（有序链表）→ two-level bin（索引数组 + 多个 OLB）→ MNode（模型 + key 数组 + vValue 版本数组）；freeze 后由并发线程 **helping** 完成 `MAKE_MODEL`（`PAGE 7` Algorithm 4）；lock-free 线性回归拟合用 CAS 或 **FAA** 累加 `Σx, Σy, Σx², Σy², Σxy`（`PAGE 8`）；`PAGE 8` Table 2 对比 FAA/CAS/Lock-Based/Hogwild!/LF 五种拟合方案，LF（分线程内存 + 单步选取）最快。
- **正确性**：`PAGE 9` 三条不变式（唯一根到键路径、变换不丢 KV、RANGE 返回全部目标 KV）+ 线性化点枚举：INSERT/DELETE 的 LP 是成功的 CAS，SEARCH 是读最新 vValue，**RANGE 是原子读 `Timestamp`**；`PAGE 9` 证明 lock freedom（读操作不阻塞也不 helping、无环）。依赖 epoch-based reclamation（DEBRA）。
- **代价**：`PAGE 4` 只假设共享内存提供 atomic read/write/CAS；无锁表、无 barrier、无锁；代价是 vValue 版本链的内存与每次更新分配；`PAGE 10` 200M 键、10M 预热、64 核 EPYC；`PAGE 12` 范围查询比 FINEdex 一致范围快约 2 个数量级。假冲突几乎为零（无锁），但 skewed 小热点下「lack of a balancing mechanism」会抖动（`PAGE 12`）。
- **与 RdmaRangeLock 的关系**：对应 **predicate / exact-range 的「无锁替代」对照组**、范围锁语义章。`PAGE 2` 与 `PAGE 13` 两处点名 XIndex/FINEdex「range queries ... are **not linearizable**」，正好解释为什么 RdmaRangeLock 必须用事务层锁而非索引层版本来保证谓词/精确范围的可串行化；`PAGE 12`「FINEdex ... **fine-grained locks** are used for inconsistent range search while **coarse-grained locks** are used for consistent range search. FINEdex consistent range queries do not scales due to their coarse-grained locks」——这是「锁粒度 × 范围查询一致性」的直接代价证据，可与 v6 层级桶锁对照。
- **可复现性**：**能**，但要改锁语义（从 `lock()` 换成版本快照），适合作为「对照组」而非同一实现版本：改 `src/index` 为 vValue + 全局 Timestamp，`SCAN` 取快照；`src/lock` 可置空（`unlock_all()` 退化）。单变量可为「范围查询一致性实现：粗粒度锁 / 细粒度锁 / 时间戳版本快照」三档，指标为吞吐、线性化违例、版本链内存。
- **关键原文证据**：
  - `PAGE 2`：「It is important to note here that **range queries in both XIndex [39] and FINEdex [29] are not linearizable**」；「without any buffer, lock, or barrier. The core of our linearizable range search is a non-trivial **versioning approach** for model arrays」。
  - `PAGE 7`：「after reading the atomic integer Timestamp, the method READ TIMESTAMP atomically increments it by 1. A range search will use this timestamp to collect the key-value pairs that have timestamps at most the one that it started with」。
  - `PAGE 8`：「This can be done via **CAS** as well as via **fetch-and-add (FAA)** primitives」（Table 2：FAA/CAS/Lock-Based/Hogwild!/LF 的回归误差与耗时）。
  - `PAGE 9`：「A **RANGE SEARCH operation linearizes on atomically reading the global Timestamp**」；`PAGE 12`：「fine-grained locks are used for inconsistent range search while coarse-grained locks are used for consistent range search」。

### 17 Kimura：现代硬件上的高效锁（2012, ADMS'12 Workshop, Kimura / Graefe / Kuno）
- **一句话问题**：当工作集全在内存时，磁盘 I/O 不再是瓶颈，`PAGE 1` Figure 1 显示 locking 占 18.7%、latching 10.2%、logging 21%、buffer pool 29.6%；如何重做「锁模块」全链路（key-range / intent / 死锁 / 早期锁释放）。
- **保护对象 / 粒度**：四个技术分属两层——① **key range 锁**（锁键值 + 键值间 gap，含不存在的键，粒度 = 半开区间 `[A,B)`）；② **意图锁**（Index/Table/Volume 粗粒度）；③ 死锁检测；④ 早期锁释放。这是本组**唯一系统讨论「锁语义」**的一篇。
- **冲突或一致性判定**：
  - `PAGE 2` Table 2：Graefe 正交模式 `N S X NS NX SN SX XN XS`，「key 模式保护已存在键值，gap 模式保护区间」，`S`=`SS`、`X`=`XX`；`NS`（key free / gap shared）与 `XN`（key exclusive / gap free）**兼容**，故 `Select ... a=15` 与 `Update ... a=10` 可并发。
  - **与用户口径直接对应**：`PAGE 2`「Mohan's and Lomet's designs employ '**next-key locking**,' i.e., a gap between keys is locked using the next-higher key value, this design uses '**prior-key locking**' instead」——gap-lock 与 next-key-lock 是同一冲突语义，只是承载键方向不同，**应归同一类**。
  - `PAGE 2` ghost records：逻辑删除只翻 ghost bit，「**the key of a ghost record does participate in key range locking just like the key of a valid record**」；`PAGE 3` fence keys（low/high，一含一排，root 为 ±∞）让跨叶页边界的范围锁不必额外 I/O。
  - `PAGE 3` Table 3 游标锁模式（升/降序 × 边界包含/排除）；`PAGE 8` Safe SX-ELR：每个 lock queue 留 `tag`（最近一次持久修改的 commit LSN），事务取锁时记录 `max tag`，只读事务提交时 `M.check_durable(xct.max tag)` 才返回——**用 tag 作为只读事务的序列化点**。
- **关键机制**：
  - ① 正交 key-range 模式 + fence keys + ghost + system transactions（系统事务只改物理表示、**不取锁**只检查冲突锁，`PAGE 3`）。
  - ② **LIL 轻量意图锁**（`PAGE 4` Algorithm 3/4）：每事务一张 **private lock table (PLT)**，全局一张 **global lock table (GLT)**，GLT 只存各模式已授权**计数**，「The GLT has **no lock queues**, thus the only inter-thread communication is a **broadcast**」；取锁是先查 PLT、再在 GLT 自旋锁临界区内 `++granted_counts[m]`；释放时 `--`、`++version`、必要时 `Broadcast`；绝对锁（S/X）请求才用 mutex + version 条件等待；死锁用超时策略。
  - ③ Dreadlocks（Bloom filter fingerprint / 按位 OR 传播 digest）+ sleep-and-backoff（释放锁时给等待线程打「digest 过期」标志，避免误判死锁）+ flush-pipeline 下 kill-short 策略。
  - ④ SX-ELR（`PAGE 8` Algorithm 6/7）：读写事务在 commit LSN 确定后即按逆序释放锁并把 `queue.tag = max(tag, commitLSN)`，只读事务按 `max tag` 等待 durable LSN。
- **正确性**：`PAGE 8` 全实验「ensure full serializability」；`PAGE 7` Table 6 给出朴素 SX-ELR 的非可串行化反例（只读事务读到尚未 flush 的 X 锁数据、崩溃后回滚但用户已看到结果）；Safe SX-ELR 依赖「删除必须留 ghost record、system transaction 不得在修改持久化前清除它」（`PAGE 8`）。`PAGE 5` 论证 LIL「can cause neither deadlocks nor long waits」，故主锁表完全不必感知意图锁。
- **代价**：`PAGE 3`「Because a cursor takes a lock for each key, the overhead to access the lock table is relatively high ... this **doubles the overhead** to access the lock table. Instead, the storage manager takes **the two locks at the same time** to reduce the overhead at the cost of slightly lower concurrency」——**锁表往返 vs 并发度的直接权衡**；`PAGE 8`「adds just one integer comparison during lock acquisition and release」；`PAGE 11` 结果：Graefe 模式比 Lomet 高 15%、比 ARIES/KVL 高 4–5×，LIL 比 SLI 再快 3–5×，SX-ELR 比 No-ELR/S-ELR 快 3–5×，整体比原 Shore-MT 最高 50×。
- **与 RdmaRangeLock 的关系**：对应 **v4 gap lock = v5 next-key lock（同一类）、v6 层级桶锁（意图锁）、锁生命周期/锁服务实现章**。`PAGE 3` 的「cursor 逐键取锁 → 锁表访问开销高」正是跨主机实测「next-key 每事务约 455.6 次 RDMA 操作」的直接解释：next-key 在范围内逐键加锁，锁数 ≈ 键数，× 每锁约 5 个 verb；而「层级锁约 51.3 次」对应意图锁/层级桶锁把多把记录锁压缩为少数粗粒度锁（≈9× 减少）。LIL 的「GLT 只存计数、无队列、broadcast」与 38 号 GLT、40 号 8-byte header、20 号 8B slot 是**同一种「抢门 + 读计数 + 读槽 + 写槽 + 释放门」锁表结构**，可解释 5 / 5.6 verb 每锁的分解。ELR 与项目「严格 2PL」冲突：ELR 在 commit log flush 前放锁，属于**弱化 2PL**，可作单变量对照但不能直接并入严格 2PL 版本。
- **可复现性**：**能**，且可分两个互不干扰的单变量：
  - 锁语义单变量：改 `src/lock` 的 `detect_conflict()` 模式表 + 边界处理（`NS/NX/SN/SX/XN/XS` 与 prior/next-key 方向、ghost/fence key 的 `key_range` 展开），接口 `lock(mode, key_range)` 不变；
  - 锁服务单变量：改 `src/lock` 的锁表实现（队列 → PLT/GLT 计数 + broadcast）、`unlock_all()` 时机（严格 2PL → SX-ELR + tag 检查）。后者会动 2PL 语义但不动冲突判定。
  - 采集：每事务锁表访问次数、每锁 verb 数、AbortCapacity、只读事务延迟、吞吐。
- **关键原文证据**：
  - `PAGE 1`：「new bottlenecks emerge: **locking, latching, logging**, and critical sections in the buffer manager ... improve all components of database locking, including **key range locking, intent locks, detection and recovery from deadlocks, and early lock release**」。
  - `PAGE 2`：「**the absence of key value 15 must be locked** for the duration of the transaction. Key range locking achieves this with a lock on a neighboring existing key value in a mode that protects not only the existing record but also the **gap** between two key values」；「'**next-key locking**' ... this design uses '**prior-key locking**' instead」。
  - `PAGE 2`：「ours uses '**ghost records**' ... the key of a ghost record does participate in key range locking just like the key of a valid record」；`PAGE 3`：「each B-tree node ... contains two **fence keys**」。
  - `PAGE 3`：「Because a cursor takes a lock for each key, **the overhead to access the lock table is relatively high** ... takes **the two locks at the same time** to reduce the overhead at the cost of slightly lower concurrency」。
  - `PAGE 4`：「The core idea of LIL is to maintain, instead of lock queues, a **set of lightweight counters that is separate from the main lock table**」；`PAGE 5`：「The GLT has **no lock queues**, thus the only inter-thread communication is a broadcast」。
  - `PAGE 7`：「This section presents a novel variant of **Early Lock Release (ELR)** ... release its locks as soon as it has formatted a commit record in the log buffer, i.e., **before it flushes**」；`PAGE 8`：「adds just one integer comparison during lock acquisition and release」。

### 38 Sherman：分离式内存写优化 B+ 树（2022, SIGMOD'22, Wang / Lu / Shu）
- **一句话问题**：内存分离（MN 近零算力）下纯单边 verb 的 B+ 树写操作因「过多往返 + 慢同步原语 + 写放大」在高争用下崩溃（复现的 FG 在 Zipf 0.99 写密集下仅 0.34 Mops、P99 近 20 ms，`PAGE 3` Table 1），如何用商用 RDMA NIC 修好。
- **保护对象 / 粒度**：**树节点级排他锁**（write-write 冲突），读用 lock-free + version（read-write 冲突）。`PAGE 6`：「we only lock at most one tree node at a time for a single write operation」。锁语义是普通互斥锁，**不含范围/谓词锁**。
- **冲突或一致性判定**：写-写：HOCL 的 `RDMA_CAS(&GLT[idx], 0, cs_id)` 抢门（16-bit masked CAS），释放用 `RDMA_WRITE` 写 0（`PAGE 6` Figure 6）；读-写：**two-level version**（叶节点 FNV/RNV 4-bit 节点级 + 每个 entry FEV/REV 4-bit 条目级，`PAGE 8` Figure 8），`Sherman_lookup` 先比 node version、再比 entry version，不一致就 `RDMA_READ` 重读（`PAGE 8` Figure 9）；4-bit 回绕用「RDMA_READ 超过 8µs 就重试」兜底。
- **关键机制（verb 与往返数，本篇是「5.6 往返/锁」的核心解释）**：
  - **基线 4 RTT**（`PAGE 4` §3.2.1）：「when modifying a tree node ... a client thread needs **4 round-trips**: ➀acquiring associated exclusive lock, ➁reading the tree node, ➂writing back the modified tree node ➃and finally releasing the lock」；`PAGE 1` 同句「lock, read, write, unlock」。FG 用 `RDMA_CAS` 抢锁、`RDMA_FAA` 释放（`PAGE 3`）。这 4 步 + 一次锁表计数/门读，正是项目「抢门 CAS、读计数、读槽、写槽、释放门」5 个单边 verb 的原型。
  - **HOCL 层级片上锁**（`PAGE 2,6,7`）：GLT 放在 MN NIC **片上内存**（ConnectX-5 256KB，`PAGE 7`），用 16-bit masked CAS 使单 MS 可容 **131,072 把锁**、`~110 Mops` CAS（`PAGE 2`）；每 CS 一张 LLT（每 local lock 8B，`n` MS 共 `n×8×131072` B），同 CS 内先取本地锁 + wait queue（FCFS 公平）再发远程 CAS，避免无效远程重试；**handover**：释放时若 wait queue 非空且 `++handover_depth ≤ MAX_DEPTH(4)`，直接把锁交给队首线程，省「至少一次往返」（`PAGE 2`, `PAGE 7`）。
  - **Command Combination**（`PAGE 8` §4.5）：利用 RC QP 的 **in-order delivery**，把「写回节点」与「释放锁」两条 `RDMA_WRITE` 合成一次 post（链表，只把最后一个标 signaled），省 1 RTT；节点分裂时若 sibling 同 MS，可把 3 条 WRITE 合并。
  - **Two-Level Version**（`PAGE 8`）：叶节点 unsorted（免 shift），无分裂时只回写 **17 字节**（16B KV + 两个 4-bit version），把写放大从整节点降到 entry 粒度。
  - B-link sibling pointer + fence keys + level 校验（`PAGE 5–6`）保证结构修改期间可达。
- **正确性**：`PAGE 8` 明写「**like FG, Sherman does not guarantee that a range query is atomic** with concurrent write operations. If upper applications ... need snapshot semantics ... they must use other mechanisms to **avoid phantom problem**」；command combination 正确性依赖 RC QP 的 in-order 属性；4-bit version 回绕依赖 8µs 超时重试。写操作互斥、读一致性由版本保证；**不保证事务可串行化**。
- **代价**：`PAGE 11` Figure 14(b) 往返分布——FG+ **94% 需 4 RTT**，Sherman **93.6% 需 3 RTT**、**3.6% 只需 2 RTT**（handover）；FG+ 的 **P99 往返 = 453**（重试风暴），Sherman P99 = 11。`PAGE 4` in-NIC 并发控制：NIC 维护 4096 buckets，同 bucket 的 atomic 串行，**单个 RDMA_ATOMIC 需 2 次 PCIe 事务**；`PAGE 10` 各技术贡献：command combination 3.37×、on-chip 3.06×、hierarchical 2.22×、two-level version 仅 3% 吞吐（但 50p 延迟降 400ns）。内存代价：entry-level version 约占叶节点 6%（`PAGE 8`）。
- **与 RdmaRangeLock 的关系**：对应 **v3 record/node 锁的跨主机实现 + 锁服务实现层（新章核心）、结构并发章**。**最直接解释「5.6 往返/锁」**：`PAGE 1/4` 给出 4-RTT 分解（lock/read/write/unlock），`PAGE 11` 给出 4→3→2 的实测分布与 P99=453 的重试放大——项目实测均值 5.6 = 固定 5 个 verb + 争用下的重试膨胀；「next-key 每事务 455.6 次 RDMA」与 FG+ P99 453 数值接近但**是不同指标**（前者是每事务 RDMA 总数，后者是单次写操作 P99 往返数），两者同源：都是高争用下逐键/逐节点加锁的重试风暴。HOCL 的 GLT 容量（131,072 = 16-bit masked CAS × 256KB 片上内存）直接对应项目「**锁表容量（槽/片）直接决定 AbortCapacity**」：槽数就是可同时持有的锁数上限。`PAGE 8` 的 phantom 声明是项目 `src/index` 只做 latch、`src/lock` 必须做范围锁的最强外部依据。
- **可复现性**：**能**，且是纯锁服务实现层单变量，`lock(mode,key_range)` / `detect_conflict()` / 严格 2PL **完全不改**，只改 `src/lock` 后端：① 锁表搬到「片上/本地 pinned 内存」；② 加 CN 本地 LLT + wait queue + handover；③ `unlock_all()` 改为与写槽合并的批量 post；④ 槽位宽 16/64-bit 与 GLT 容量。采集：每锁 verb/RTT 分布、远程 CAS 重试数、NIC IOPS、P99、AbortCapacity vs 槽数。
- **关键原文证据**：
  - `PAGE 1`：「modifying an index node ... always requires **multiple round trips (i.e., lock, read, write, unlock)**」；「introduces a **hierarchical lock** that exploits **on-chip memory** of RDMA NICs」。
  - `PAGE 2`：「Global lock tables are stored in the **on-chip memory of RDMA NICs**, thus eliminating PCIe transactions ... (~110 Mops)」；「a thread can **hand over** its acquired lock to another thread directly, **reducing at least one round trip** for acquiring global locks」。
  - `PAGE 4`：「**4 round-trips**: ➀acquiring associated exclusive lock, ➁reading the tree node, ➂writing back the modified tree node ➃and finally releasing the lock」；「a single RDMA_ATOMIC needs **two PCIe transactions**」。
  - `PAGE 5`：「We record a **sibling pointer** for every leaf node and internal node as in the **B-link tree**」（承接 18 号）。
  - `PAGE 7`：「in the NIC we use (i.e., ConnectX-5), **256KB on-chip memory** ... an MS can maintain **131,072 locks** in its GLT」；Figure 6：`MAX_LOCK_PRE_MS = 131072`、`MAX_DEPTH = 4`、`RDMA_CAS(&GTL[...], 0, cs_id)`。
  - `PAGE 8`：「**Sherman does not guarantee that a range query is atomic** ... they must use other mechanisms to avoid **phantom problem**」；「command combination ... combines multiple RDMA_WRITE commands ... **one round trip is saved**」。
  - `PAGE 11`：「**94% of write operations need 4 round trips in FG+, while 93.6% need 3 round trips in Sherman** ... 3.6% ... only need 2 round trips ... for FG+, the **99th percentile of round trips is 453**」；`PAGE 12`：「Sherman only needs to write back **17 bytes**」。

### 40 DecLock：解耦锁 / 队列通知（2025, arXiv:2505.17641, Zhang et al.）
- **一句话问题**：锁与数据同放在 MN 时，CAS 自旋锁在高争用下把 MN-NIC IOPS 吃光——`PAGE 1`「with 256 clients, each client averages **28 retries** to acquire a lock, consuming **over 95% of MN-NIC IOPS**」，吞吐跌到理想的 5% 以下、P99 涨 69×；如何把「所有权转移」与「状态维护」解耦。
- **保护对象 / 粒度**：一把 **reader-writer 锁**（共享/排他），由应用关联到被保护对象（可嵌入 data entry）。锁语义是标准 RW-lock 互斥 + FIFO/task-fair，**不含范围/谓词语义**。
- **冲突或一致性判定**：`PAGE 4` CQL 把状态拆成「原子控制面（8-byte header：`qhead`/`qsize`/`wcnt`/`reset_id`）+ 非原子数据面（循环队列 entry：mode/cid/version/timestamp）」。取锁用一次 **FAA** 同时 `qsize+=1, wcnt+=EX(mode)`，由 FAA 返回的旧 header 判定能否立即获得所有权；`PAGE 16` 附录 A 证明互斥：只要 `wcnt>0` 写者排他（Invariant 2.1），只要 `qsize>0` 写者不能抢读者（Invariant 2.2）。队列 entry 用 **version**（`VERSION(idx) = idx / QUEUE_CAPACITY`）识别过期/未填充条目，不一致就重取（额外 READ ≤1.8%）。
- **关键机制（verb 与往返数）**：
  - `cql_acquire`（`PAGE 5` Figure 7）：① `FAA(HEADER_ADDR)` 入队并返回旧 header；② 若需等待，`WRITE(QADDR(i), {mode, cid, VERSION(i)})` 填充自己的 entry；③ `wait_for_notification()` 纯 CN↔CN 消息，**不再打 MN**。
  - `cql_release`：⑥ `FAA` 出队（`qhead+=1, qsize-=1, wcnt-=EX`）；⑦ `READ` 整条队列（与 FAA 一起 piggyback 隐藏延迟）；⑧–⑲ 按 successor 模式 `send_notification` 给写者或一批相邻读者。
  - `PAGE 1`「requiring **at most two remote operations to acquire a lock**」、`PAGE 4`「at most two (and typically just one) remote operations towards MNs」；`PAGE 10` 实测「**at most 1.10 RDMA operations on average for acquisition**」，且「regardless of the critical section length」。
  - **TS-based hierarchical locking**（`PAGE 7–9`）：每 CN 一张本地锁表（mutex/state/holder_cnt/cql_held/wq），只有本地尚未持有 CQL 锁时才打 MN；队列 entry 记录 16-bit 获取时间戳，释放时比较本地/远程 waiter 的 TS 决定「直接本地移交」还是「先释放 CQL 让远程 waiter 拿」，用 timestamp 保跨 CN 公平（task-fair / phase-fair 两档）；`PAGE 9` 用 MN 上一个 FAA 计数器做 CN 间时钟同步（精度 < 3µs）。
  - **Lock Reset**（`PAGE 6`）：队列溢出/version 回绕/CN 故障时，CAS 设 reset ID → 广播 reset 并收集响应 → 2 条 WRITE 重置 header/queue；每个 CN 维护 reset counter 丢弃过期通知。
- **正确性**：`PAGE 7` §4.5 + `PAGE 16` 附录 A 证明 **mutual exclusion + liveness**（无故障时）；`PAGE 7` §4.6：CN/MN/网络故障下保互斥，MN 故障不保强 liveness；公平性在故障下不保。`PAGE 9` 明确「timestamp comparisons occasionally produce wrong results ... only affects fairness ... **does not compromise correctness**」。依赖 RC、应用限时释放锁、queue capacity ≥ 最大客户端数。
- **代价**：`PAGE 3` 自旋锁基线「平均 CAS 重试 37.8 次/获取、吞吐从 1.61 Mops 跌到 0.20 Mops」；`PAGE 10` 长临界区下 CASLock/DSLR+/ShiftLock 平均重试 **387.9 / 349.2 / 15.6** 次，DecLock ≤1.10；`PAGE 10` 吞吐提升：对 CASLock 43.37×、DSLR+ 4.35×、ShiftLock 1.81×；P99 降 98.2% / 67.8% / 43.7%。内存：本地锁表每 CN <20MB（`PAGE 7`）；CQL 队列容量 = CN 数（默认 8），`PAGE 11` 容量到 128 时释放延迟仍 <8.80µs。代价是 **CN↔CN 通知依赖网络**：`PAGE 17` 附录 C，CN-CN 延迟升高时 DecLock 吞吐最多降 14.7%（ShiftLock 降 49.6%）。
- **与 RdmaRangeLock 的关系**：对应 **v3 锁 + 锁服务实现层（C9 队列通知）、跨主机矩阵**。**第二篇直接解释「5.6 往返/锁」**：它给出「抢门」从 CAS 自旋（28 / 37.8 / 387.9 次重试）换成 FAA 入队 + 通知后的 ~1 次远端操作，量化了重试膨胀如何把「固定 5 个 verb」放大成实测 5.6+；`PAGE 12` 集成进 Sherman 后吞吐比 Sherman-NH 高 27.71×、比 Sherman 高 2.31×，说明**换锁服务实现不动索引语义**就能改写跨主机矩阵。层级本地锁表（每 CN <20MB）+ CQL 队列容量 = CN 数，对应 v6 层级桶锁与「锁表容量 → AbortCapacity」。注意：DecLock 是 RW 锁而非范围锁，`detect_conflict()` 仍是二进制兼容判定，可作 RdmaRangeLock `lock()` 的下层锁服务。
- **可复现性**：**能**，纯锁服务实现层、不改 `lock(mode,key_range)` / `unlock_all()` / 严格 2PL 语义。改 `src/lock` 后端：CAS 自旋门 → 8-byte header FAA + 队列 WRITE/READ + CN↔CN 通知；再加 CN 本地锁表 + timestamp 公平策略（task-fair / phase-fair / local-bound / remote-prefer 四档单变量）。采集：每锁平均 RDMA 操作数、重试次数、MN-NIC IOPS 占用、P50/P99、AbortCapacity、公平性（同锁获取顺序偏差）。
- **关键原文证据**：
  - `PAGE 1`：「with 256 clients, each client averages **28 retries** to acquire a lock, consuming **over 95% of MN-NIC IOPS**」；「requiring **at most two remote operations to acquire a lock**」。
  - `PAGE 2`：「**cooperative queue-notify locking (CQL)** ... enqueues itself to a centralized array (on MNs) and waits for **notification** from the client that releases the lock (between CNs)」；「encodes the queue metadata and the lock mode into an **8-byte atomic header** ... using a single **fetch-and-add (FAA)** operation. Since FAA operations always succeed」。
  - `PAGE 3`：「the average number of **CAS retries per acquisition rises to up to 37.8**」；`PAGE 4` Fig.4「atomic control plane / non-atomic data plane」。
  - `PAGE 5` Figure 7 `cql_acquire`：`hdr = FAA(HEADER_ADDR(lid)) [qsize+=1, wcnt+=EX(mode)]`；`WRITE(QADDR(...), {mode, cid, VERSION(i)})`；`wait_for_notification(lid)`。
  - `PAGE 10`：「DECLOCK requires at most **1.10 RDMA operations on average for acquisition** ... CASLock (**387.9**), DSLR+ (349.2), and ShiftLock (15.6)」；`PAGE 12`：「improves throughput by up to **27.71× compared to Sherman-NH and 2.31× compared to Sherman**」。
  - `PAGE 7`：「each lock consists of a **CQL lock on the MN** ... and a **local lock on each CN**」；`PAGE 12`：「at most **0.0014% of acquisitions are reset**」。

### 20 LOTUS：锁解耦到计算池（2025, arXiv:2512.16136, Hu et al.）
- **一句话问题**：DM 事务系统把锁与数据都放 MN，导致 MN RNIC 被单边 CAS/FAA 打满（`PAGE 3`：单 RNIC 的 8B `RDMA_WRITE` 上限 35 Mops，而 8B **CAS 仅 2.5 Mops**）；把锁整体挪到 CN 能否既消瓶颈又保一致。
- **保护对象 / 粒度**：记录级 **读锁/写锁**（MVCC 下读写集加锁）；插入额外锁 **index bucket**（把远端索引桶地址当 key 加锁）。锁语义是标准 RW + 2PL 式加锁阶段，**不含 key_range/谓词语义**（index bucket 锁是「防同桶并发插入」的粗粒度替代）。
- **冲突或一致性判定**：`PAGE 4` Algorithm 1：锁表是定长哈希表，**每 slot 8B = 7B fingerprint + 1B counter**，每 8 个 slot 组成一个 lock bucket；counter=1 表示写锁、非零偶数表示读锁；bucket 满 / 冲突 / 读锁计数溢出即失败；`lock state` 记录 holder 的 txn ID / CN ID / mode 以保证幂等；最终用 **CAS** 原子取得锁。`PAGE 8` SR 靠「读写数据取写锁、只读数据取读锁」保证 Tstart→Tcommit 间数据不变；SI 关掉只读数据的读锁。只读事务不取锁，用 **cacheline version (CV)**（`PAGE 9`）判一致性。
- **关键机制（verb 与往返数）**：
  - **Lock disaggregation**（`PAGE 1,3`）：锁表放 CN、数据放 MN，锁操作完全在计算池内完成；本地锁直接执行本地 CAS，远程锁走 **RDMA SEND/RECV RPC**（注意：这是 two-sided，与项目「单边 RDMA」设定有张力）；`PAGE 5`「multiple remote lock requests targeting the same CN from a transaction are **batched into a single RDMA message**, saving IOPS」。
  - **Application-aware sharding**（`PAGE 5`）：主键里用户指定 critical field 的最低 12 位作 shard number（其余 52 位哈希），TPCC 用 W_ID、TATP 用 subscriber ID；两级负载均衡 = hybrid transaction routing（只读随机、读写路由到 shard 所属 CN）+ pass-by-range resharding（过载 CN 把热 shard 的**锁所有权**转给最闲 CN，只转所有权不搬数据，`PAGE 6`）。
  - **Lock-first transaction protocol**（`PAGE 7–8` Figure 10）：把加锁作为每个 execution phase 的第一步，早判冲突早 abort；Execution = Lock Data → Read CVT → Read Data；Commit = Write Data & Log（version 设为 `INVISIBLE`）→ Get Tcommit → Write Visible → Unlock Data（**远程 unlock 发出即返回、不等 ACK**）。`PAGE 10` 明写「read-write transactions involve **four steps (locking, reading, writing, and unlocking)**」。
  - **VT cache**（`PAGE 7`）：CN 缓存 CVT，写锁请求时顺带失效远端 CVT，「zero consistency overhead」。
  - **Lock-rebuild-free recovery**（`PAGE 8–9`）：锁视为 ephemeral，CN 挂了不重建锁表，只让存活 CN 扫描日志、放行已进入 commit 的事务、abort 其余并从各自 lock state 释放故障 CN 持有的锁。
- **正确性**：`PAGE 8` 支持 SR 与 SI、ACID；SR 论证「all read-write transactions can be viewed as executing serially at their commit timestamps」，只读事务用「movable Tstart」插入串行序；依赖 read lock 省略读集验证、依赖 ghost/CV 处理无锁只读一致性；`PAGE 8` resharding 期间用「先停服务、等旧锁释放（超时 10ms 主动 abort）、再交接」保证原子性。
- **代价**：`PAGE 11` 消融：`+Lock Sharding` 在 TPCC +9.9%、SmallBank +29.7%，但 **TATP −10.8%**（只读事务多、RPC 吃 CN CPU）且 P99 变差（散锁需跨多 CN）；`+Two-Level Load Balancing` +25.7%/+8.4%/+36.5%；`+Version Table Cache` +5.9%/+7.6%/+20.1%。`PAGE 10` 锁表 32MB、VT cache 4.5MB/CN（< 工作集 1.3%）——**锁表容量是硬上限**。`PAGE 12` 对比「理想化 RDMA 锁」（每 CN 本地计数、仅 0→1/1→0 时发 FAA）：「LOTUS outperforms this idealized baseline by **1.3–1.9×** ... these locks still incur RNIC contention on the MNs due to RDMA Atomic operations」。整体比 Motor/FORD 吞吐最高 2.1×、延迟降 49.4%（`PAGE 1`）。
- **与 RdmaRangeLock 的关系**：对应 **v3 锁 + 锁服务实现层（C10 锁表位置）、谓词/插入保护、跨主机矩阵**。`PAGE 5` 的 **index bucket 锁**是本组最接近「谓词/间隙保护」的实现：它锁「桶地址」而非键，等价于把「可能插入某区间的所有键」压成一把粗粒度锁——可作为 RdmaRangeLock `predicate` / `exact-range` 的一个「哈希桶级粗粒度」单变量对照（假冲突高但 verb 少）。`PAGE 3` 的 CAS 2.5 Mops vs WRITE 35 Mops 解释了为何「抢门 CAS」是 5 个 verb 里最贵的一个；`PAGE 10` 的四步（locking/reading/writing/unlocking）与 38 号 4 RTT 完全对应。`PAGE 12` 的「理想化 RDMA 锁」= 本地计数 + 0→1/1→0 时一次 FAA，正是「减少每锁 verb」的理论下界，可解释层级锁 51.3 次/事务。**张力提示**：LOTUS 用 two-sided SEND/RECV RPC 做远程锁，而 RdmaRangeLock 设定是单边 RDMA；复现时需把 RPC 换成单边 verb 或显式标注该差异。
- **可复现性**：**能**，锁服务实现层单变量，`lock()` 冲突语义基本不变（但 lock-first 把加锁提前到执行阶段第一步，改的是执行时序而非 `detect_conflict()`）。改 `src/lock`：① 锁表从 MN 搬到 CN（本地哈希 + 分片映射）；② 批量远程锁请求；③ `unlock_all()` 异步化；④ 分片数/critical field/锁表容量（32MB 可调）。采集：MN-NIC IOPS、CN CPU、每事务 RDMA 数、RPC 批大小、AbortCapacity vs 槽数、resharding 中断时长。`PAGE 9` 的 `Begin()/AddRO()/AddRW()/Execute()/Commit()` 接口与项目 `lock()/unlock_all()` 不同，若照搬协议需扩展接口，但**纯锁表位置/分片实验可保持现有接口**。
- **关键原文证据**：
  - `PAGE 1`：「The key innovation of LOTUS is to **disaggregate locks from data and execute all locks on CNs**, thus eliminating the bottleneck at MN RNICs」；「when lock acquisition fails for one data record, the entire transaction aborts, necessitating the release of all previously acquired locks. This process results in **substantial waste of network bandwidth**」。
  - `PAGE 3`：「a single RNIC ... up to **35 Mops for RDMA WRITE (8B)**, while the maximum IOPS for **CAS (8B) is only 2.5 Mops**」；「FORD ... **doorbell-batched RDMA CAS+READ** to lock and read data within a single round-trip time」。
  - `PAGE 4`：「Each slot in the hash table is **8B** ... a 7B **fingerprint** ... and a 1B **counter**. Every 8 slots form a **lock bucket**」；「a **CAS** instruction is used to atomically acquire the lock」。
  - `PAGE 5`：「for **insert operations, locking is required for both the key and the bucket address** in the remote index (referred to as the **index bucket**) ... By locking the index bucket, LOTUS ensures concurrency safety for insert operations」；「multiple remote lock requests targeting the same CN ... are **batched into a single RDMA message**, saving IOPS」。
  - `PAGE 7`：「A distinctive feature of our protocol is its **separation of the locking phase as the first step** in each execution phase」；`PAGE 8`：「returns the result immediately after issuing remote unlock requests, **without waiting for their completion**」。
  - `PAGE 10`：「read-write transactions involve **four steps (locking, reading, writing, and unlocking)**」；「we limit the **lock table ... to 32MB** and the version table cache ... to 4.5MB per CN」。
  - `PAGE 12`：「LOTUS outperforms this **idealized RDMA locking** baseline by 1.3–1.9× ... they still incur **RNIC contention on the MNs due to RDMA Atomic operations**」；`PAGE 13`：「DSLR [94], ShiftLock [21], and **DecLock [96]** ... keep lock states in MNs and rely on RDMA Atomic; In contrast, LOTUS ... treats locks as **ephemeral** entities managed within CNs」。

---

## 1. 本组发展史时间线（按年份给因果链）

主线：**每一步都从「一次 `lock()` 的远程路径」里再删掉一个 verb / 一次往返，而锁语义始终没变**；副线是结构层 latch 与事务层 lock 的分工被反复确认。

- **1981 — Lehman & Yao，B-link（18 号）**：起因是磁盘页模型下「查找拿到旧父指针 → 读到已被分裂的节点」的错误（`PAGE 6` 反例）。解法 = 每节点一个 `link` 右链 + `high key`，**查找零锁**、更新最多 3 把锁、良序加锁防死锁。因果意义：第一次把「结构并发」从「事务锁」里剥离出来，right-link/high-key 成为此后所有分布式索引（含 FG、Sherman）分裂期间的标配；`PAGE 18` 的 ghost 脚注埋下逻辑删除的种子。
- **1993 / 2007 — Lomet / Graefe 的 key-range 模式（经 17 号转述）**：`PAGE 2` 指出 ARIES/KVL 用 next-key、Lomet 把锁模式拆成 range/key 两段但缺 `RangeS-N`，Graefe 给出正交模式集。因果意义：把「不存在的键」纳入保护对象，gap-lock / next-key-lock 成型——RdmaRangeLock v4/v5 的语义祖先。
- **2001 / 2010 — Cha 等 OLFIT / Graefe B 树锁综述（经 19 号转述）**：`PAGE 6`「lock coupling simply does not scale on modern multi- and many-core CPUs」「OLFIT uses versions to detect changes within a node ... uses the B-link tree idea」。因果意义：多核 cache-line 失效把「锁本身」变成瓶颈，版本校验 + right-link 成为结构层标准组合。
- **2012 — Kimura / Graefe / Kuno，ADMS（17 号）**：起因是 `PAGE 1` Figure 1「内存常驻后 80% CPU 在 B-tree/latching/logging/locking/buffer pool，且单线程」。四项改造：**正交 key-range + fence keys + ghost**（提并发）、**LIL 轻量意图锁**（缩短锁表临界区：队列→计数器 + PLT/GLT + broadcast）、**Dreadlocks + sleep-backoff**（死锁检测）、**Safe SX-ELR**（把锁持有期从「commit log flush 后」提前到「形成 commit 记录后」，用 lock queue tag 修只读事务可串行化）。因果意义：现代硬件上第一次系统性地「优化锁模块本身」，并留下 `PAGE 3`「cursor 逐键取锁 → 锁表访问开销高 → 一次取两把锁省往返」这一**锁服务实现层的原始权衡**；ELR 是「锁生命周期」这一维度的起点。
- **2016 — Leis 等，ART OLC / ROWEX（19 号）**：`PAGE 2` 细粒度读写锁即使无逻辑冲突也因 cache line 失效不伸缩，于是用 64-bit version + CAS 升级（OLC）或「读者零校验、写者原子保证」（ROWEX）。`PAGE 1` 脚注正式把结构层 latch 与事务层 lock 分家。因果意义：确认「结构层可以完全不用阻塞锁」，为 38 号 two-level version 与 15 号无锁范围扫描铺路。
- **2019 — FG（Ziegler 等，经 38 号复现）**：第一棵可部署在 DM 上的纯单边 B+ 树，`RDMA_CAS` 抢锁 + `RDMA_FAA` 释放 + B-link + checksum 无锁读；`PAGE 3` Table 1 显示写密集 + 倾斜下崩溃到 0.34 Mops / P99 20ms。因果意义：**「过多往返 + 慢同步原语 + 写放大」三病因被明确定义**，直接触发 Sherman。
- **2020 — XIndex（35 号）**：学习索引要支持并发写，用 group + **delta index** + Two-Phase Compaction（merge 放引用、RCU barrier、copy 换真值）+ per-record lock/version；`PAGE 3` 指出「barrier 阻塞更新」是缺陷。因果意义：结构层「后台改结构不阻塞前台」的成熟方案，同时暴露「范围查询不 linearizable」。
- **2022 — Sherman（38 号）**：三管齐下——**HOCL**（GLT 进 NIC 片上内存、LLT 本地委托 + wait queue + handover，MAX_DEPTH=4）、**Command Combination**（RC in-order，写回 + 解锁合并，4→3 RTT，handover 再 →2）、**Two-Level Version**（entry 级版本，17B 回写）。`PAGE 11`：FG+ 94% 4 RTT / P99 453，Sherman 93.6% 3 RTT / 3.6% 2 RTT / P99 11。因果意义：**把「每锁 verb / 往返数」变成可测量的优化对象**；`PAGE 8` 明确「范围查询原子性/防幻读不由索引负责」，从架构上坐实 `src/index` 与 `src/lock` 分工。
- **2023 — Kanva（15 号）**：`PAGE 2` 直指 XIndex/FINEdex「range queries ... are not linearizable」，用 vValue 版本链 + 全局 `Timestamp` 把范围查询线性化点定在「原子读 Timestamp」，全程 CAS/FAA、无锁无 barrier。因果意义：给出「不用锁也能做可串行化范围查询」的对照路线，反衬事务层范围锁的必要性。
- **2023–2025 — 锁服务专门的演化（经 20/40 号引用）**：Citron（FAST'23 单边范围锁）、DSLR（SIGMOD'18 ticket lock）、lock fission（OSDI'24 网内锁管理）、**ShiftLock（FAST'25，handover / MCS）**。因果意义：锁从「索引内部的一个字段」独立成「可被替换的锁服务」，`PAGE 3` 的 40 号指出 ShiftLock 仍平均 2.34 次检查、MCS 读锁靠 counter 造成公平性问题。
- **2025 — DecLock（40 号）**：把「所有权转移」彻底移出 MN——CQL 用 8-byte header 的**单次 FAA** 入队 + 队列 entry WRITE/READ + **CN↔CN 通知**替代 CAS 自旋；`PAGE 10` 平均 ≤1.10 RDMA 操作/获取（对比 CASLock 387.9 次重试）；TS 分层锁保跨 CN 公平。因果意义：**队列通知替代 CAS 自旋**，把每锁远端操作压到 ~1。
- **2025 — LOTUS（20 号）**：再进一步，**锁表整体搬到 CN**（lock disaggregation），MN 侧完全不再有锁相关 RDMA Atomic；app-aware 分片 + 批量远程锁请求 + lock-first 协议 + lock-rebuild-free recovery；`PAGE 12` 连「理想化 RDMA 锁（本地计数 + 0→1/1→0 单次 FAA）」都被超越 1.3–1.9×，因为后者仍要在 MN 管全局状态。因果意义：把「锁表放哪」推到极端（MN → NIC 片上 → CN 本地），完成锁服务实现层的因果闭环。

**一句话因果链**：磁盘页结构错误 →(1981 right-link/high-key)→ 结构 latch 与事务锁分家 →(1993–2007 gap/next-key 语义)→ 多核使锁表/临界区成为瓶颈 →(2012 LIL 计数器化 + ELR 缩短持锁期)→ 版本化乐观读 →(2016)→ DM 近零算力使单边 verb 的 4-RTT 成为瓶颈 →(2019 FG 崩溃)→ 片上锁表 + 命令合并 + handover →(2022 Sherman, 4→3→2 RTT)→ 队列通知消除自旋 →(2025 DecLock, ~1 op)→ 锁表整体搬出 MN →(2025 LOTUS, 0 次 MN 锁操作)。

---

## 2. 锁服务实现层可复现类别（[REPRO]，单变量消融）

> 共同前提：以下类别**均不改 `lock(mode, key_range)` 的冲突语义与 `detect_conflict()`**，只改 `src/lock` 后端（锁表放哪、一次 `lock()` 展开成哪些 verb、`unlock_all()` 何时发）。对应综述新增「锁服务实现层」章与现有跨主机实测矩阵（每锁约 5 个单边 verb / 实测均值 5.6 往返；next-key ≈455.6 次/事务、层级 ≈51.3 次/事务）。
> 建议统一采集的基线指标：`per_lock_verbs`（按 verb 类型分桶：CAS/FAA/READ/WRITE）、`per_lock_rtt`、`remote_retry_count`、`MN_NIC_IOPS`、`P50/P99`、`abort_rate`、`AbortCapacity`（逐步增大并发事务直到锁表耗尽）、`lock_table_bytes`。

### [REPRO-1] 本地锁委托 / 本地排队（CN 本地锁表 + wait queue）
- **来源**：38 号 HOCL 的 LLT（每 local lock 8B、FCFS wait queue，`PAGE 6–7`）、40 号 CN 侧 local lock table（每 CN <20MB，`PAGE 7`）、20 号 CN 本地锁表（`PAGE 4`）。
- **要改的模块**：`src/lock` 前端——在 `lock()` 之前加一层「本地哈希桶 + 等待队列」，同进程/同 CN 内先排队，只有本地无持锁者时才发远程抢门；`unlock_all()` 释放后唤醒队首。
- **要采集的指标**：远程 CAS/FAA 重试次数、每锁 RTT、P99、MN-NIC IOPS、队列等待时长。
- **与跨主机矩阵对应**：把「5.6 往返/锁」中的无效重试削掉，对应 Sherman 4→3 RTT 中 hierarchical 部分的 2.22× 吞吐（`PAGE 10`）；与「层级锁 51.3 次/事务」同向（把多把记录锁委托给少数本地代表）。
- **是否需要改接口**：**否**，`lock()`/`unlock_all()`/`get_lock_count()` 不变（队列是内部状态）。

### [REPRO-2] handover（锁移交 + 最大连续移交深度）
- **来源**：38 号 `MAX_DEPTH = 4` 的 handover（`PAGE 6` Figure 6、`PAGE 7`）、40 号 local-prefer / local-bound 策略（`PAGE 11`）、20 号 pass-by-range resharding（锁所有权转移，`PAGE 6`）、ShiftLock（FAST'25）。
- **要改的模块**：`src/lock` 的 `unlock_all()` 路径——解锁时不写远端释放门，若本地 wait queue 非空且 `handover_depth ≤ D` 就把 ownership 直接交给队首；`D` 为单变量（0/1/2/4/∞）。
- **要采集的指标**：2-RTT 比例（Sherman 实测 3.6%）、handover 深度分布、跨 CN 公平性偏差、P99、饿死率。
- **与跨主机矩阵对应**：直接复现 Sherman「至少省一次往返」→ 每锁 5.6 → 2 的尾部；`D=∞` 可观察跨主机饿死（对应 40 号 `PAGE 11` local-prefer 在 WO 负载的尾延迟）。
- **是否需要改接口**：**否**（但 `get_lock_count()` 需反映「本地已移交但未远程释放」的锁，属内部计数口径，建议记录为指标而非改语义）。

### [REPRO-3] 批量解锁 / 命令合并（doorbell batching、in-order 合并、异步 unlock）
- **来源**：38 号 Command Combination（RC QP in-order，写槽 + 释放门合并为一次 post，只 signaled 最后一个，`PAGE 8`）、20 号「batched into a single RDMA message」（`PAGE 5`）与「unlock 发出即返回不等 ACK」（`PAGE 8`）、20 号转述 FORD 的 doorbell-batched CAS+READ（`PAGE 3`）、17 号「一次取两把锁省锁表往返」（`PAGE 3`）。
- **要改的模块**：`src/lock` 的 verb 发射层——把 `lock()` 内的「抢门 CAS + 读计数」、以及 `unlock_all()` 的「释放门」与数据写槽合并成链表一次性 post；`unlock_all()` 改为异步（不等 CQ）。
- **要采集的指标**：每锁 verb 数、每锁 RTT、CQ 完成项数/信号数、解锁 ACK 延迟、吞吐。
- **与跨主机矩阵对应**：解释 5 个固定 verb 如何被合并成 3 个 RTT（Sherman `PAGE 11`）；`unlock_all()` 天然是批量接口，是项目接口相对「逐把 unlock」的原生优势，单变量 = 批量大小。
- **是否需要改接口**：**否**；甚至 `unlock_all()` 比逐把解锁更契合该优化。

### [REPRO-4] 锁分片 / 锁表容量（槽/片 → AbortCapacity）
- **来源**：38 号 GLT `MAX_LOCK_PRE_MS = 131072`（16-bit masked CAS × 256KB 片上内存，`PAGE 6–7`）、20 号 8B slot / 8-slot bucket / 32MB 锁表 / 12-bit shard number（`PAGE 4–5,10`）、40 号 queue capacity = CN 数（`PAGE 7,11`）、17 号 PLT/GLT 计数表（`PAGE 4`）。
- **要改的模块**：`src/lock` 的锁表容量与哈希分片数 `S`（单变量扫描 `S` = 桶数 / 槽位数 / 分片数），以及 slot 位宽（16/64-bit）。
- **要采集的指标**：**AbortCapacity**（并发事务数 → 因锁表满/哈希碰撞而 abort 的拐点）、假冲突率（同桶不同键误判）、锁表内存/片上内存占用、远程 atomic IOPS。
- **与跨主机矩阵对应**：**直接对应项目「锁表容量（槽/片）直接决定 AbortCapacity」**；可复现 Sherman 131,072 锁 vs LOTUS 32MB 锁表两种容量口径，并与 v6 层级桶锁的桶数扫描合流。
- **是否需要改接口**：**否**（`get_lock_count()` 正好用来观测已用槽数）。

### [REPRO-5] 片上锁表模拟（on-chip / device memory 锁表）
- **来源**：38 号 HOCL（首个用商用 NIC 片上内存存锁，`PAGE 7`：「To the best of our knowledge, Sherman is the first RDMA-based system that leverages on-chip memory of commodity RDMA NICs」）、20/40 号的对照（锁留在 MN DRAM → RNIC 争用）。
- **要改的模块**：`src/lock` 后端地址空间——把锁表从远端 DRAM 改注册到「NIC device memory / 本机 pinned 免 PCIe 路径」；单变量 = 锁表所在介质（远端 DRAM / 片上模拟 / CN 本地 DRAM）。
- **要采集的指标**：单次 CAS 延迟与吞吐（对照 `~110 Mops` vs `PAGE 3` 的 2.5 Mops CAS）、PCIe 事务数、P99「抢门」延迟、冲突 CAS 排队时间。
- **与跨主机矩阵对应**：解释 5 个 verb 里「抢门 CAS」为何最贵（`PAGE 4` 单个 atomic 需 2 次 PCIe 事务；`PAGE 3` CAS 2.5 Mops vs WRITE 35 Mops）；对应 Sherman on-chip 的 3.06× 吞吐贡献（`PAGE 10`）。
- **是否需要改接口**：**否**，但需改内存注册/后端初始化（工程上属于「改后端不改接口」）。

### [REPRO-6] 队列通知替代 CAS 自旋（CQL / queue-notify）
- **来源**：40 号 CQL（FAA 8-byte header 入队 + 队列 WRITE/READ + CN↔CN 通知，`PAGE 2,4,5`）、38 号 LLT wait queue 的 FCFS 通知（`PAGE 7`）、20 号 SEND/RECV RPC 锁请求（`PAGE 5`）。
- **要改的模块**：`src/lock` 的抢门原语——`CAS 自旋重试` → `FAA 入队 + 填 entry + 等待通知`；释放时 `FAA 出队 + READ 队列 + notify successor`；再加 8-byte header 位域（qhead/qsize/wcnt/reset_id）与 entry version。
- **要采集的指标**：**每锁平均 RDMA 操作数**（目标复现 ≤1.10 vs CASLock 387.9 重试）、MN-NIC IOPS 占用、P50/P99、公平性（获取序偏差）、reset 频率、通知对 CN-CN 延迟的敏感度（40 号 `PAGE 17` 附录 C 最多降 14.7%）。
- **与跨主机矩阵对应**：**最直接解释 5.6 往返/锁的重试膨胀部分**——把「固定 5 verb」在争用下的放大压回 ~1；对应 40 号对 Sherman 的 2.31× 提升（换锁服务不动索引语义）。
- **是否需要改接口**：**否**，`lock()`/`unlock_all()` 签名不变；若引入 task-fair/phase-fair 两档可作第二单变量。

### [REPRO-7] 锁表位置解耦（MN-side ↔ NIC 片上 ↔ CN-side）
- **来源**：38 号（MN NIC 片上 GLT）、40 号（MN 上 CQL 队列 + CN 本地锁表）、20 号（锁整体在 CN、MN 无锁）。
- **要改的模块**：`src/lock` 的部署目标与路由层——单变量 = 锁表归属（MN DRAM / MN NIC 片上 / CN 本地 + shard-to-CN 映射）；本地/远程锁请求走不同路径。
- **要采集的指标**：MN-NIC 锁相关 IOPS、CN CPU、跨 CN RPC 消息数、每事务 RDMA 数、吞吐/延迟、故障恢复时长（对照 20 号 233ms 恢复）。
- **与跨主机矩阵对应**：三档分别对应 Sherman / DecLock / LOTUS 的实测数量级；解释「层级锁 51.3 次/事务」为何远低于 next-key（锁被本地化 + 分片后远程 verb 趋零）。
- **是否需要改接口**：**纯位置实验否**；若照搬 LOTUS 的 `lock-first` 协议与 `Begin/AddRO/AddRW/Execute/Commit` 接口则**需要扩展接口**（且引入 two-sided RPC，与项目「单边 RDMA」设定冲突，须显式标注为「协议级对照」而非 verb 级消融）。

### [REPRO-8] 锁生命周期：严格 2PL vs 早期锁释放（ELR）
- **来源**：17 号 §5 Safe SX-ELR（`PAGE 7–8`，lock queue tag = commit LSN，只读事务 `check_durable(max tag)`）。
- **要改的模块**：`src/lock` 的 `unlock_all()` **调用时机**（commit 后 → 形成 commit 记录后）+ 锁队列 tag 采集 + 只读事务 durable-LSN 检查；`detect_conflict()` 不变。
- **要采集的指标**：平均持锁时长、冲突 abort 率、只读事务延迟、吞吐（17 号 SX-ELR 3–5×）、可串行化校验（用 `PAGE 7` Table 6 的反例做回归测试）。
- **与跨主机矩阵对应**：解释「锁数 × 每锁 verb」之外的第三个因子——**持锁时长**决定冲突窗口；与项目「严格 2PL」直接对照，是语义边界实验。
- **是否需要改接口**：**否**（`unlock_all()` 仍是同一个调用，只是调用点提前；tag 为内部状态），但**会弱化 2PL 语义**，须在综述里与「严格 2PL」版本明确分栏，不能并入同一实现版本。

### [REPRO-9] 结构层 latch/version 与锁层的边界（对照组，非锁服务消融）
- **来源**：18 号 right-link + high key + 有序 latch、19 号 OLC/ROWEX 64-bit version、35 号 per-record version、38 号 two-level version、`PAGE 8` phantom 声明、15 号无锁版本化范围扫描。
- **要改的模块**：**只改 `src/index`**（latch coupling → 版本校验 / right-link 右移 / entry 级版本 / vValue 快照），`src/lock` 保持不动，用于证明「结构层再强也不能防幻读」。
- **要采集的指标**：SMO 期间读重试次数、latch 争用、cache-line 失效、范围查询线性化违例数、假冲突。
- **与跨主机矩阵对应**：为项目「`src/index` 只允许 latch、`src/lock` 只允许 lock」提供消融证据；15 号可作为「去掉 `src/lock` 后范围查询是否仍可串行化」的阴性对照。
- **是否需要改接口**：**否**（`src/lock` 一行不改；这是分层合法性验证而非锁服务优化）。

> 优先级建议：**REPRO-4（锁表容量→AbortCapacity）、REPRO-3（批量解锁）、REPRO-6（队列通知）** 与本组跨主机实测矩阵咬合最紧，应先进综述消融表；REPRO-1/2/5 作为 Sherman 三件套的逐项拆解；REPRO-7/8 标注为「协议/语义级对照」；REPRO-9 作为分层合法性回归。
