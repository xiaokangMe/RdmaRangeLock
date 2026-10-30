# M1 接口与区间锁表示

M1 只保留 deneva 中与范围锁实验直接相关的抽象：`idx_key_t`、`txnid_t`、`RC`、`KeyRangeEnt` 和 `KeyXidCache` 的方法命名。RDMA 传输、远端偏移、工作负载、行对象和具体并发控制算法暂不引入。

## 区间

`KeyRangeEnt` 使用显式的端点存在性和开闭属性，支持点区间、闭区间、半开区间、开区间及左右无界区间。两参数构造函数保留 deneva 的兼容约定：`EndKey == 0` 表示点 key；新代码应优先使用命名工厂，避免把 0 当作哨兵。

区间相交按端点包含性判断。例如 `[10,20)` 与 `[20,30)` 不相交，而 `[10,20]` 与 `[20,30)` 相交。空区间不会产生锁。

## 锁表

`LockManager` 是线程安全、无等待的区间锁表。锁冲突时返回 `Abort` 并可返回冲突事务 ID；同一事务的重复加锁幂等，精确区间支持 `S -> X` 升级。`LockKind` 记录 `Record`、`Gap`、`NextKey`、`Predicate` 和 `Node`，为 v3-v6 在同一接口上实现不同语义预留空间。

`KeyXidCache` 是 deneva 风格的兼容 facade，内部仍使用 `LockManager`，因此 workload 迁移时不需要引入 deneva 的全局状态。

## 验证

`test/m1_range_lock_test.cpp` 覆盖边界、无界区间、共享/排他冲突、同事务升级和 `KeyXidCache` 兼容接口。使用 CMake 生成 `rrl_lock` 和 `rrl_m1_range_lock_test`，Debug 与 Release 均通过 CTest。