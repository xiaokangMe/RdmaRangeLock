# Separator/key-prefix 锁复现方案（2026-11-08）

## 文献精读结论

本次精读了以下材料：

1. Graefe, A Survey of B-tree Locking Techniques, ACM TODS 2010，§5.2–§5.4。
2. Graefe, Hierarchical Locking in B-tree Indexes, BTW 2007 章节。
3. Mohan, ARIES/KVL: A Key-Value Locking Method for Concurrency Control of Multiaction Transactions Operating on B-Tree Indexes, VLDB 1990, pp.392–405。
4. Lomet, Key Range Locking Strategies for Improved Concurrency, VLDB 1993, pp.655–664。
5. Graefe, Hierarchical Locking in B-tree Indexes, US 7,953,717 / US 2011/0208704 A1（用于核对 separator-key 的工程细节）。

当前工作区没有找到上述论文作者公开的可直接移植实现。搜索到的普通 B+ 树、课程锁管理器和通用 B-tree 项目没有论文中的 separator 逻辑、SMO 锁复制/合并语义，不能直接移植为算法实现。Deneva 源码也没有该协议。

## 论文机制

B+ 树内部节点的 separator key s 代表从 s（包含）到下一个 separator（不包含）的逻辑键范围。锁资源至少由：

    (index_id, tree_level, logical_separator_key)

唯一确定，不能只用物理 page id。这样 separator 在页分裂、合并或负载均衡后迁移时，原有逻辑保护仍然有效。

separator 层级采用多粒度意向锁：

    祖先 separator: IS / IX
    实际覆盖的 separator 范围: S / X / SIX
    记录: S / X

较高层 separator 可覆盖多个低层 separator，减少扫描锁数量；边界叶页仍需要细粒度覆盖。分裂时，原锁范围必须复制到新 separator；合并时，需要把两侧锁集合合并，若合并后的集合存在不兼容模式，则延迟结构合并，不能丢锁。

这与固定桶不同：

- 固定桶按 key / bucket_width 划分，与树结构无关；
- separator 锁的边界来自实际 B+ 树 separator；
- separator 锁对象是逻辑键和层级，不是物理页 ID；
- separator 锁保护的是由父节点分隔键定义的子树范围。

## 统一架构适配

本项目保持 IRangeLockManager 公共签名不变，新增内部 hierarchy_layout=separator：

- 本地路径使用同一 LockManager；
- RDMA 路径使用同一 RemoteLockManager 区间槽和 native txn-state；
- B+ 树只提供结构快照与覆盖范围，事务锁仍在 src/txn/；
- 不复制第二棵索引；
- 不把 B+ 树结构 latch 当作事务锁；
- 事务仍然 strict 持锁到 commit/abort。

第一阶段采用“逻辑 separator 范围”的安全适配：快照返回的 separator 覆盖区间作为稳定逻辑锁范围，锁对象不依赖易变物理页号。该适配保留论文的覆盖关系和分层粒度，但明确记录与原始系统在锁表资源 ID 和 SMO 实现上的差异。只有完成分裂复制/合并兼容检查后，才允许称为 separator-key 复现。

## 不变式与验收

1. 每个 separator 资源的覆盖范围必须是合法半开区间，且同层相邻范围不重叠。
2. 对任意扫描，所选 separator/叶边界范围的并集必须覆盖查询区间及插入空隙。
3. 点写必须取得祖先 IX 和记录 X；范围扫描取得祖先 IS 和覆盖 S；混合访问使用 SIX。
4. separator 快照在事务锁获取后必须重新计算并验证，树结构变化导致覆盖变化时中止当前尝试。
5. 分裂不得缩小已有事务的逻辑覆盖；合并不得合并出不兼容的锁集合。
6. 空结果、首尾无穷、跨叶、叶分裂、叶合并、删除边界和 ABA 交错必须分别验证。
7. 本地与 RDMA 使用相同的 separator 计划和兼容规则；旧 bucket/leaf/dag 结果不混组。
8. 先通过单线程/双线程交错和真实 RDMA 回环正确性，再建立新的性能配置；不修改旧矩阵数据。

## 当前状态

本文件只记录论文与实现方案。未启动正式矩阵、构建或 CTest。论文 PDF 已在本机 papers/ 保存，原始来源和复现差异必须在后续代码提交中继续保留。

## Correctness fixes in this pass (2026-11-08)

- Fixed the asymmetric left-endpoint containment rule: an open outer fence cannot contain a closed inner fence at the same key; a closed outer fence can contain an open inner fence.
- Separator plans now acquire locks in actual B+ tree depth order: root intention at phase 0, separator/leaf nodes at depth+1, and record locks at the maximum unsigned phase. This preserves top-down acquisition and takes record locks last.
- A non-leaf root is no longer repeated as a phase-1 separator resource; a single-page leaf root remains because it is the only scan-cover object.
- Tests cover open/closed boundaries, empty-result ranges, ranges adjacent to UINT64_MAX, SIX joining for mixed range-read plus point-write transactions, and rejection of stale plans after page split and deletion topology changes.
- Validation: cmake --build build-shared -j8 succeeded; the focused test passed; ctest --test-dir build-shared --output-on-failure reported 54/54 passed. Twenty-one RDMA/native environment tests were skipped under the existing environment rules and are not counted as executed RDMA validation.
