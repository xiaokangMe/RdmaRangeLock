# src/index

索引结构的实现。本目录承担两个版本：

| 版本 | 文件（待实现） | 说明 |
|---|---|---|
| v1 | `v1_btree_single.cpp` | 单线程 B+ 树，作为基线。提供 `insert / delete / lookup / scan_range` |
| v2 | `v2_btree_latch_coupling.cpp` | 在 v1 的页布局上加入 latch coupling（crabbing） |

## 约定

- 两个版本**共用同一套页布局和 key 比较逻辑**，来自 `include/index/btree.h`，避免"结构差异"污染性能对比
- 页大小、填充因子、key 类型通过配置文件指定，不写死在代码里（配置文件放 `experiments/configs/`）
- **不在这里做锁**。事务级锁一律在 `src/lock/`，本目录只有 latch

## v2 的正确性要点

latch coupling 的释放条件写错是最常见的 bug：

1. 父节点 latch 只能在"确认子节点安全"之后释放
2. "安全"的判定依赖操作类型：插入看是否可能分裂，删除看是否可能合并
3. 必须在压力测试下验证结构不变式（树高、key 有序、兄弟指针连通）

对应测试：`test/stress_latch_coupling.cpp`
