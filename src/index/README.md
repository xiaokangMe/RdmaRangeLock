# src/index

索引结构的实现。本目录承担 v1 和 v2 两个版本，二者共用 `include/index/btree.h` 中的 `BTreePage` 布局和 key 比较规则。

| 版本 | 文件 | 说明 |
|---|---|---|
| v1 | `v1_btree_single.cpp` | 单线程 B+树，支持插入、删除、点查和范围扫描 |
| v2 | `v2_btree_latch.cpp` | shared/exclusive page latch coupling，支持并发读写 |

## v1

- `order` 可配置，内部页最多保存 `order - 1` 个分隔 key；
- 叶页保存 key/value，并由 `prev/next` 串成顺序链；
- 叶分裂、内部页分裂和根分裂；
- 重复 key 更新 value，不增加 `size()`；
- 显式开闭区间的点查和范围扫描；
- 删除叶页中的 key，并刷新祖先分隔 key；
- `validate()` 检查 parent 指针、key 有序性、分隔 key、叶链和元素计数。

## v2 latch coupling

读路径按父页 shared latch、子页 shared latch、释放父页的顺序向下；写路径按同一方向持有 exclusive latch。叶链扫描在切换页前先锁住 next 页，避免释放当前页后 next 页被分裂。根指针由 root guard 保护，根分裂时临时取得 exclusive root guard。

写事务目前使用 writer mutex 串行化结构修改，读线程仍可通过 page latch 并发执行；这是为了先固定正确性和锁顺序，后续可在不改变页布局的情况下消除写者串行化。

对应测试：`test/v2_btree_latch_test.cpp`，包含 4 个写线程、4 个读线程、重复 100 次运行和 AddressSanitizer 验证。

两个版本都不包含事务级锁。事务锁统一放在 `src/lock/`，后续版本必须继续复用本目录的页布局。