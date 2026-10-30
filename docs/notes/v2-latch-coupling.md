# v2 latch coupling 设计记录

## 目标

在 v1 的 `BTreePage` 布局上增加结构并发保护。latch 只保护内存结构的一次访问，不能替代事务级范围锁。

## 加锁顺序

- 读：root guard（shared）→ 当前页（shared）→ 子页（shared）→ 释放父页；
- 写：writer gate → root guard（shared 或根分裂时 exclusive）→ 当前页（exclusive）→ 子页（exclusive）；
- 叶链扫描：先取得 next 页 shared latch，再释放当前页。

所有路径都自顶向下，避免父子反向等待。根替换必须持有 root guard exclusive，保证读者不会看到半完成的根分裂。

## 为什么暂时保留 writer gate

v2 首先验证页级 latch 的生命周期和结构不变式。写者 gate 让多个结构修改不会同时改动同一父页，降低第一版实现的变量；读者不拿 writer gate，因此范围扫描仍能与写事务并发。后续可把 writer gate 收窄到根分裂和父页修改路径，再测量写扩展性。

## 验证

`test/v2_btree_latch_test.cpp` 使用 4 个写线程插入 2048 个 key，同时 4 个读线程执行点查和短范围扫描。Debug/Release CTest 通过；远端重复运行 100 次通过；AddressSanitizer 构建通过。