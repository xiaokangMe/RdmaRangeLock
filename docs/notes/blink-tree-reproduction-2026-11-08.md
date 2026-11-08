# B-link tree 复现记录（2026-11-08）

## 论文依据

本实现依据 Lehman–Yao，Efficient Locking for Concurrent Operations on B-Trees（1981）。论文 PDF 保存在本机项目的 papers/Lehman-Yao-1981-Efficient-Locking-Blink-Trees.pdf。

B-link 节点增加 high key 和右兄弟指针。查找或下降发现 key 大于等于 high_key 时沿右链接继续；分裂先发布右节点、右链接和左节点的 high key，再把 separator 插入父节点。结构 latch 只保护一次结构修改，事务级范围锁仍由 src/lock/ 和 src/txn/ 独立管理。

## 本地实现

BTreeBlink 位于 src/index/v3_blink_tree.cpp，共用 BTreePage、key 比较和 BTreeIndex 接口。普通叶页插入只取得叶页 exclusive latch；叶分裂、内部节点分裂、根创建和根收缩在 root guard 的结构临界区完成。每次结构变化重建各层右链接和 high key；查找、范围扫描、key-neighbor 与层级覆盖接口均能沿 B-link 右链完成边界纠正。

删除沿现有项目的空叶摘除和根收缩语义执行，不引入 sibling redistribution；这保持了综述项目已有删除模型，并在结构验证中检查父子关系、叶深度、叶链、右链接和 high-key 边界。

## 远端 RDMA 页格式

OffsetBTreePage 已加入 right_link、high_key、has_high_key，格式版本从 2 升为 3。TreeCore::descend、叶覆盖和层级覆盖在 high-key 越界时沿远端右链接纠正；远端分裂先把右节点的继承 fence 写入，再发布左节点的右链接和 high key，然后更新父 separator。叶页的 B-link 右链与原有 next/prev 叶链保持一致，RDMA 验证会拒绝缺失 fence、孤立右链和不一致叶链。

远端数据树仍使用项目已有的共享结构 gate 来保护批量结构写入；B-link 元数据提供了论文要求的路由和 split 发布顺序，但不把当前 RDMA 数据路径宣称为完全无结构 gate 的无锁实现。

## 验证

- test/blink_tree_test.cpp：多次叶/内部页分裂、high-key 边界查找、跨叶范围扫描、并发插入与查找、删除、空叶摘除和结构验证。
- test/rdma_btree_core_test.cpp：远端偏移树分裂、记录与 tombstone、范围边界、右链纠正和故障传播。
- rrl_workload_runner --index blink：本地 YCSB-A/E、TPC-C Payment/New-Order 可复用统一 workload 驱动；RDMA 协议仍要求 --index rdma，不会把本地 B-link 当成远端索引。
