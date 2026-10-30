# Berenson et al. 1995：隔离级别与范围锁依据

论文：Hal Berenson, Philip Bernstein, Jim Gray, Jim Melton, Elizabeth O'Neil, Patrick O'Neil, “A Critique of ANSI SQL Isolation Levels”, SIGMOD Record 24(2), 1995。PDF 已保存到本机 zongshu 项目的 `papers/berenson-1995-isolation-levels.pdf`。

## 对本项目的结论

论文把 dirty read、non-repeatable read、phantom 和 serializability 分开讨论。只锁住一次范围查询返回的记录，可以阻止已有记录被修改，却不能阻止另一个事务插入满足谓词的新记录；因此 repeatable read 式记录锁仍可能产生 phantom。要阻止这种插入，需要锁住谓词对应的 key range、索引间隙，或在提交时做等价的谓词冲突检测。

这直接对应本项目的 v3/v4：v3 使用 `LockKind::Record`，固定测试中 T2 可以插入 15；v4 额外使用 `LockKind::Gap` 锁住 `(10,30)` 和边界 gap，T2 对 15 的插入意向发生冲突。

论文讨论的是隔离语义，不规定 B+树页布局。实现仍沿用 deneva 的有序 key 思路和本项目的 `KeyRangeEnt`，把谓词范围映射为显式开闭区间。