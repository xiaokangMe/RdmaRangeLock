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

写路径使用页 latch coupling，根指针变更由 root guard 保护；已有普通 update 快路径不再依赖整树 writer mutex。

对应测试：`test/v2_btree_latch_test.cpp`，包含 4 个写线程、4 个读线程、重复 100 次运行和 AddressSanitizer 验证。

两个版本都不包含事务级锁。事务锁统一放在 `src/lock/`，后续版本必须继续复用本目录的页布局。

## 共享数据单边 RDMA 后端

`rdma_btree.cpp`/`rdma_btree_core.h` 实现注册内存内的偏移 B+ 树和 uint64 记录。线格式统一定义在公共 `include/index/btree.h`，所有 RDMA 锁协议复用同一实现。按 Deneva 的 root/child/next offset 方式寻址，节点和记录经 RDMA READ/WRITE 访问，线上不存客户端的 C++ 指针。

服务端 CPU 只负责初始化和连接；客户端每次短操作通过远端结构共享/排他 latch 保护读写。写者公布 writer 位后阻止新的读者，等待已存在读者退出；这是结构 latch，不跨事务持有。页分裂保持有序和双向叶链；删除标记 record.valid=0，重新插入复用记录，不回收页。结构故障或不确定写入置 poison 并使测量 fail-stop。

该后端仍有整个写批次的串行点，不是 Sherman 的 HOCL 实现。数据层不含事务级锁；严格 2PL、策略、提交许可和缓冲写入统一由 src/txn 与 src/lock 负责。所有扫描/页路径缓冲在 Begin 前分配。测试覆盖 order 3/4/16/128 的多级分裂、tombstone、uint64 边界、读写故障，以及真实 RoCE 多客户端共享记录和精确累计值。


返回值生命周期：远端 lookup 的 void* 指向本 worker 的单值缓冲，下一次 lookup 会复用；scan 返回值指向该 worker 的扫描缓冲，下一次扫描会复用。统一事务驱动立即读取值、只在下一次扫描前保留 key，不把临时值地址当成记录身份或跨操作缓存。需要长期保存结果的调用方必须自行复制值，不能保留这些指针。
