# v6 层级锁阅读与实现笔记

## 论文依据

多粒度锁的模式采用 Gray 等人提出的意向锁思想：访问子对象前，事务先在祖先对象声明 IS 或 IX，再在目标对象申请 S 或 X。IS/IX/S/X 的兼容矩阵写入 `include/common/types.h`，锁升级使用模式 join，保证同一事务从 IS 到 IX 或从 S 到 X 时仍保持严格两阶段锁。

范围锁的语义基础来自 Berenson 等人的 SIGMOD 1995 论文：层级粒度只能减少锁的数量，不能放松谓词保护；目标桶仍需覆盖扫描范围，才能阻止冲突插入。

## 轻量实现

公共 BTreeIndex 接口不暴露页 ID，因此 v6 使用稳定的逻辑桶表示节点：根节点是无界 `LockKind::Node` 区间，key 按 `hierarchy_bucket_width` 映射到闭区间桶。范围读加 Root IS 与覆盖桶 S；点读同样使用 IS+S；写入、更新和删除使用 Root IX 与桶 X。根上的 IS/IX 是意向锁，与子桶 S/X 不直接冲突；同一桶的 S/X 仍冲突。

实现位置：`src/txn/workload.cpp::run_hierarchical_lock`。`--bucket` 参数可调桶宽，便于测量锁数量与冲突率的变化。测试覆盖根意向锁兼容、不同桶并发、同桶 X 冲突以及 IS→IX 升级。

## 限制

这是在 M1 统一区间接口上的逻辑层级近似，不读取 B+树内部页号；因此它用于比较多粒度锁的协议开销，不宣称已经实现具体存储引擎的页级锁。锁表仍采用 no-wait 冲突返回 Abort。

## 桶宽权衡

桶越宽，范围读需要的 bucket 锁越少，但不同 key 落入同一桶后会增加粗粒度冲突。固定 YCSB-E（1000 keys、200 transactions、4 threads、range=16）得到：bucket=16 时 2226 次加锁、2 次冲突；bucket=64 时 1677 次加锁、7 次冲突；bucket=256 时 1570 次加锁、13 次冲突。数据保存在 `experiments/results/v6_bucket_sweep.csv`。这说明 v6 的收益是锁元数据开销下降，代价是粒度变粗后的冲突上升。
