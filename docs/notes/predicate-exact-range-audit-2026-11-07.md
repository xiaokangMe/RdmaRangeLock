# predicate / exact-range 实现溯源审查（2026-11-07）

审查对象：`.66:~/RdmaRangeLock` HEAD `db4a728`，全程只读（`git show/log/grep` + `cat` + `python3 -` 读取 CSV），未修改远端任何文件。
目的：回答「这两个协议是按论文复现的，还是项目自造的」。

## 一、结论（先给判定）

**不是论文复现，是项目自造的最小化实验对照。** 但也不是"瞎做"：它有清晰的工程纪律（严格 2PL、同一棵 B+树、同一 workload、独立 `LockKind` 防串组、边界单测），能正确防幻读，可作为单变量基线使用。

三点必须写清楚：

1. **两个协议在实现语义上完全等价，只差一个 `LockKind` 标签。** 把它们当作两个不同类别（报告的 C4 / C5）在锁语义层面站不住。
2. **概念上能对应到文献，但没有任何一篇论文的机制被真正实现，且代码/笔记/正文里没有一处引用出处。**
3. `exact-range` 是「工业系统明确做不到的理想化上界」，不是任何系统的复现；本地正式矩阵尚未启动。

## 二、代码事实

### 2.1 predicate 的来源与实现

- 首次出现在 commit `837fadf`（「新增三种可切换冲突策略并保证撤销与提交及严格释放的有序性」），只是 `enum class PolicyProtocol { Record, Gap, NextKey, Hierarchy, Predicate }` 里的一个值。
- 实现只有两处（当前 `src/txn/policy_workload.cpp`）：
  - `:248` 范围读 `acquire(LockMode::S, KeyRangeEnt::half_open(op.key, op.end_key), LockKind::Predicate)`
  - `:300` 点操作 `acquire(read ? S : X, KeyRangeEnt::point(op.key), LockKind::Predicate)`
- 冲突判定完全交给 `include/common/range.h:100` 的 `RangeLock::conflicts()`：`txn_id 不同 && range.overlaps() && lock_modes_conflict()`。
- `LockKind::Predicate` 这个命名来自 `docs/notes/m1-interface.md`：「只保留 **deneva** 中与范围锁实验直接相关的抽象…`LockKind` 记录 Record、Gap、NextKey、Predicate 和 Node，**为 v3-v6 在同一接口上实现不同语义预留空间**」——是借用开源框架的枚举名，不是文献命名。
- `docs/notes/rdma-native-policy-batch-2026-11-06.md:18` 对它的全部描述：「predicate | RemoteLockManager 区间槽 | **单谓词区间保持原语义**」。

### 2.2 exact-range 的来源与实现

- commit `1022bc6`「实现精确区间锁本地与 RDMA 路径」，diff 仅 61 行插入 / 14 行删除，核心改动是：
  - `types.h`：`LockKind` 加 `ExactRange`，注释「A single lock for the exact logical query interval. **Kept distinct from Predicate so experiments can compare the two representations and unlock/cache matching never aliases them accidentally.**」
  - `policy_workload.cpp:248`：在 predicate 那行 `if` 后加一个 `||` 分支，同样的 `LockMode::S`、同样的 `half_open(lo,hi)`，只换 kind。
  - `policy_workload.cpp:300`：三元表达式加一行 `ExactRange`。
  - `workload_runner.cpp`：白名单 + 映射两行；`test/m1_range_lock_test.cpp`：6 行边界测试（`[60,80)` 的 S 与 `point(70)` 的 X 冲突、`point(80)` 的 X 不冲突，验证半开上界）。
- commit message 自述：「**复用**本地与远端区间锁表、批量发布和严格释放路径」。
- `docs/notes/exact-range-lock-2026-11-06.md` 原文最关键一句：「**两者都保护完整谓词区间**，但 ExactRange 使用独立的 kind、缓存键和槽记录，可单独测量锁表表示与冲突行为。**未启动正式矩阵。**」

### 2.3 为什么两者等价

`LockKind` 在冲突判定里只服务于 `Node`：`range.h:103-117` 的 `parent_intention`（unbounded 根意向例外）与 `bucket_intention`（桶意向 vs 记录锁例外）都只对 `Node` / `Record` 生效。`lock_manager.cpp:37`、`remote_lock_manager.cpp:397` 同样是 `Node` 特判；`wait_policy.cpp:77`、`wait_control.cpp:130` 只是把 `ExactRange` 当作枚举上界校验。其余按 kind 比较的地方（`lock_manager.cpp:112/151/158/182/190`、`txn_lock_cache.h:69/80/95`、`remote_lock_manager.cpp:38/1000`）都是「同一事务的 owned-slot / unlock / 缓存键匹配」，属命名空间隔离，不改变跨事务冲突语义。

**即：跨事务的加锁、冲突、中止、释放行为，Predicate 与 ExactRange 逐位相同。**

### 2.4 实测佐证

跨主机 `smoke-not-formal-cross-host-20261106-*.csv` 共 61 对同配置格子（`source_commit`、`campaign_config_sha256`、run、seed、range、zipf、workload、threads、wait_policy 等全部一致，`source_commit = 4066274…`）：

- 两协议均满足 `lock_acquires == operations`（例：`220563/220563`、`249859/249859`），即每个操作恰好一把锁，范围读是**一整段区间**、不按行/键展开。
- 无系统性差异：`throughput_committed_txn_s` 例 `2172.01` vs `2158.24`、`2477.58` vs `2499.52`、`2299.96` vs `2470.63`，差异方向不一致，与实验本身的随机波动同量级；`lock_conflicts` 有 20/61 格完全相同。
- 这不是「两个算法性能接近」，而是「同一个算法跑了两遍、各带独立命名空间」。

## 三、文献对照：对应什么、又缺了什么

| 项目实现 | 可对应的文献 | 缺失的机制 |
|---|---|---|
| `predicate`：读侧登记 `[lo,hi)` S，写侧登记 `point(k)` X，冲突 = 区间重叠 + 模式不兼容 | **Eswaran 1976 谓词锁的一维单区间退化**：「A Lock on P by T conflicts with Lock on Q by U if … P and Q are **mutually satisfiable**」（Fekete 讲义 PAGE 3）。单区间下「区间相交」恰好就是 satisfiability 判定，故退化实现是自洽的 | 谓词表达式与解析、多谓词/布尔组合（covering predicates, DNF）、谓词-谓词双向检查、satisfiability 判定器、谓词挂到索引节点做剪枝。Eswaran 原设还指出一般 satisfiability 不可判定、Fekete 直言「**Never implemented in any system I know of**」（PAGE 3） |
| 同上 | **MySQL 5.7 WL#6609 MBR predicate lock 的一维退化**：官方博客 PAGE 11「there is no such absolute ordering concept … difficult to define the 'next' key … we might simply lock the **MBR** used for the query」。一维键空间上 MBR 退化为 `[lo,hi)`（G2 笔记 PAGE 10–12） | 谓词挂在 R-tree / B+树节点上、插入时沿访问路径检查谓词、锁随页分裂迁移。Graefe 讲义 PAGE 4「Attach query predicates to all nodes searched. Inserters check predicate locks attached to leaves that they visit」也没做 |
| `exact-range`：精确 `[lo,hi)`，端点不必是现存键 | **Lomet 1993 / SQL Server `RangeS-S` 的「单区间锁」概念**，但那两者边界是索引键、且带复合模式与意向锁 | 复合 `(RangeMode,KeyMode)`、instant 检查、边界键命名、SMO 时锁区间分裂/合并 |
| 同上 | **更准确的定位是 MySQL 8.4 §17.7.3 的反面对照**：「InnoDB **does not remember the exact WHERE condition, but only knows which index ranges were scanned**」（G2 PAGE 1）。即精确 WHERE 区间是工业系统明确做不到的**理论理想化上界** | 无系统可复现；它是本项目定义的理想基线，不是实现 |

**出处标注情况**：远端 `docs/survey/` 与全部 `docs/notes/` 中，这两个协议的说明**没有引用任何论文或官方文档**（grep `Eswaran|Lomet|WL#6609|MBR|precision lock|Gray` 在 survey 下只命中 `05-hierarchical-locking.md:7` 一条待办：「多粒度锁（Gray 等）的核心思想」）。综述 `04-record-and-range-locking.md` 只有 exact-range 一段、**没有 predicate 小节**；`06` 章横向表只加了 exact-range 一行。而 04 章那句「精确区间锁把**整个逻辑谓词**直接作为一个区间对象发布」，字面就是 predicate 的定义，却未说明两者关系。

## 四、对综述的直接影响（必须改）

1. **分类要合并或真正做开**。当前 C4（精确/任意区间锁）与 C5（谓词/精度锁）在实现上是同一个 `lock(S, [lo,hi))`。要么在正文里合并为一类「整段逻辑区间锁」，把 `LockKind` 说明成「同一算法的两种锁表命名空间，用于隔离实验组」；要么给 predicate 补上文献里的关键机制（谓词挂 B+树节点 + 插入时按访问路径检查谓词），让它产生可测的剪枝/假阳性差异，那时才配分成两类。
2. **不能写「复现了谓词锁」**。建议措辞：「本项目的 `predicate` 是 Eswaran 谓词锁在**单区间谓词**下的退化实现，等价于 InnoDB WL#6609 MBR 谓词锁在一维键空间上的退化；`exact-range` 与之语义相同，仅锁表 kind 不同，作为独立命名空间的对照。二者均非任何系统的完整复现。」
3. **明确 exact-range 的理想化性质**：补一句「InnoDB 只记录扫描过的索引范围，不记精确 WHERE 条件」，把它定位成上界基线。
4. **按仓库红线处理文献性论断**：`CONSTRAINTS.md` 要求「没有对应实现的论断不写进文章」，因此上述文献对应必须单列「相关工作」小节，与实测章节分开，并补 `[@key]` 引用（远端目前还没有 `docs/refs/references.bib`）。
5. **补数据**：本地 `exact-range` 正式矩阵为 0 格（61 行全来自 `rdma-exact-range`），且 design note 明说「未启动正式矩阵」。在补齐 60 格本地几何之前，正文不能给这两个协议的定量结论。

## 五、如果要让 C4 / C5 真正分开（最小方案）

- `predicate` 保持"精确区间"，另加一个 `predicate-attached` 变体：把查询区间登记到本次扫描访问过的 B+树叶节点 id 上（现有 `hierarchy_cover_into()` / `leaf_cover.h` 已能给出叶覆盖），插入/更新时只检查其查找路径经过的节点上挂的谓词。这是 Graefe 讲义 PAGE 4 与 WL#6609 的真实做法，会引入「假阳性（同叶但不同区间）」这一可测代价。
- 单变量 = 谓词挂载开关；指标 = 锁表条目数、冲突检查次数、假冲突率、跨主机 `rdma_operations`。
- 这样 C5 才有独立语义，C4 仍是「精确 WHERE 区间」理想上界，两者对比才有内容。

## 附：核查命令与证据位置

- 远端：`git show 1022bc6`、`git show 837fadf`、`cat docs/notes/exact-range-lock-2026-11-06.md`、`docs/notes/m1-interface.md`、`docs/notes/rdma-native-policy-batch-2026-11-06.md`、`sed -n '1,40p' docs/survey/04-record-and-range-locking.md`、`include/common/range.h:100-119`、`src/txn/policy_workload.cpp:248/300`、`test/m1_range_lock_test.cpp` diff。
- 本地文献证据：`docs/notes/reading-2026-11-07/G1-classic-locking.md`（Eswaran / Fekete 讲义 PAGE 2–5）、`G2-industrial-hierarchy.md`（WL#6609 PAGE 10–13、MySQL 8.4 §17.7.3 PAGE 1–2、Graefe 讲义 PAGE 4）。
- 原始 PDF：`D:\codex\zongshu\papers`，索引 `00-INDEX.md`。
