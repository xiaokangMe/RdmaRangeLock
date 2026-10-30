# v4 Gap Lock

v4 在 v3 记录锁基础上增加 gap 保护。范围读先枚举结果，再取得：

- 范围内部的开区间 Gap S 锁；
- 左右边界的点 Gap S 锁；
- 返回记录的 Record S 锁。

插入请求取得目标 key 的 Gap X 锁；如果目标 key 落入已有范围的 gap，LockManager 的区间相交判断返回 `Abort`。写集合仍然延迟到 commit，所有锁在 commit/abort 后统一释放。

当前实现使用同一 `LockManager` 和 `LockKind::Gap`，冲突判定仍以显式区间相交为核心。它是一个清晰的 v3 对照实现，后续 v5 会把 record + gap 合并成 next-key 区间，减少调用层的组合规则。

`test/v4_gap_lock_test.cpp` 验证 T1 的 `(10,30)` gap 锁阻止 T2 插入 15；释放 T1 锁后，T2 才能插入。

## 统计口径

本版本同样采用 no-wait 冲突返回 `Abort`，不能把 abort 数直接称为阻塞率；实验结果应同时报告冲突数和提交吞吐。
