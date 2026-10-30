# src/index

索引结构的实现。本目录承担 v1 和 v2 两个版本，二者共用 `include/index/btree.h` 中的 `BTreePage` 布局和 key 比较规则。

| 版本 | 文件 | 说明 |
|---|---|---|
| v1 | `v1_btree_single.cpp` | 单线程 B+树，支持插入、删除、点查和范围扫描 |
| v2 | `v2_btree_latch_coupling.cpp` | 在 v1 页布局上加入 latch coupling（待实现） |

## v1 已实现能力

- `order` 可配置，内部页最多保存 `order - 1` 个分隔 key；
- 叶页保存 key/value，并由 `prev/next` 串成顺序链；
- 叶分裂、内部页分裂和根分裂；
- 重复 key 更新 value，不增加 `size()`；
- 显式开闭区间的点查和范围扫描；
- 删除叶页中的 key，并刷新祖先分隔 key；
- `validate()` 检查 parent 指针、key 有序性、分隔 key、叶链和元素计数。

v1 不包含事务级锁，也不包含 latch。事务锁统一放在 `src/lock/`，后续版本必须继续复用本目录的页布局。