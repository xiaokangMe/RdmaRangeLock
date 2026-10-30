# Comer 1979：B-tree 设计要点与 v1 对照

阅读论文：Douglas Comer, “The Ubiquitous B-Tree”, ACM Computing Surveys, 11(2), 1979。PDF 保存在本机 zongshu 项目的 `papers/comer-1979-ubiquitous-b-tree.pdf`。

## 对 v1 的直接影响

- B-tree 的关键目标是让每个节点容纳多个 key，降低树高和外存访问次数；v1 将 `order` 定义为内部页最大 child 数，页最多保存 `order - 1` 个 key。
- 查找沿内部页的分隔 key 向下，叶页保存实际 key/value；范围查找从目标叶页开始沿兄弟指针顺序扫描。
- 插入先在叶页内按序放置，溢出时分裂为左右两个页，并把右页的最小 key 上推到父页；父页继续溢出时递归分裂，根分裂产生新根。
- 删除首先删除叶页中的 key/value，并刷新受影响的父页分隔 key。v1 当前不做合并或重新分配，后续在需要衡量删除代价时再补充。
- 论文强调节点容量、分裂和合并是性能与空间利用率的主要权衡；因此 v1 的 order 走实验配置，不写死在实现中。

## 与本项目接口的关系

论文讨论的是索引结构，不涉及事务级范围锁。`BTreeIndex` 只提供 `insert`、`erase`、`lookup` 和 `scan_range`；`LockManager` 通过 `KeyRangeEnt` 表达逻辑区间，不读取 B+树页内部字段。这样 v1/v2 的结构对比不会把事务锁实现混入索引代码。

## 当前限制

v1 是单线程正确性基线，删除不合并下溢页，也没有 latch。后续 v2 会在同一 `BTreePage` 布局上加入自顶向下 latch coupling；v3 以后再把区间锁接到事务骨架。