# 按实际祖先路径消除全树事务结构屏障

用户要求不把全树 topology S/X 当作最终限制；继续参照 Deneva 的叶 X、分裂父节点 CAS 和树下降方式修正移植。

源码核查：system/txn.cpp:5045 的 try_insert_new_data 在无需分裂时 CAS 目标叶；需分裂时 CAS 父节点，父满返回 Abort。实际远端父未满分支也提前 return Abort，后面的分裂代码不可达；正常插入分支在函数返回前就释放叶字，不能直接满足本项目严格 2PL。移植的是叶/祖先保护机制，补齐递归分裂与整个事务提交后释放。

方案：索引只返回真实节点身份、实际祖先路径、路由 fence、占用和点键存在性，不获取事务锁。所有访问按逻辑 root→实际 root→内部祖先→实际叶→record 加锁；内部用 IS/IX。普通 Insert 取叶 X；可能分裂时，对能容纳本事务最坏分裂数的最低安全祖先取 X；根可能增长则逻辑 root X。Delete 对能承受最坏删除数的最低安全祖先取 X，否则逻辑 root X。这样新叶/新父发布后仍在旧的安全祖先 X 之下，发布和事务释放之间不会出现保护空窗。

先汇总最终模式后获取，不持锁升级。规划和加锁间结构可能变化，因此拿齐锁后重新从树生成计划，要求新计划的每个真实节点/记录及模式都被已持计划覆盖，覆盖失败正常 abort/retry；不得使用旧叶快照。安全祖先 X 与其他事务沿实际路径持有的 IS/IX 冲突，保护整批写回期间的新节点及删除后重路由的键域。跨叶结构事务只阻塞涉及的子树，无全树 topology 对象。

原索引层的短期结构 latch 仍由索引负责，禁止把事务锁调用搬入索引。后续普通不分裂插入也要用叶级短 latch，而非把全局结构排他 latch 隐藏成已消除。验证需包括：暂停一个叶的结构事务时另一独立子树可完成；真实分裂/根增长；新叶已发布但事务尚未释放时其他读取仍阻塞；规划后发生分裂时旧路径必须拒绝；三策略和双机精确和值及小时稳定性。禁止启动完整正式矩阵。

## 用户要求的兼容路径

在 strict 祖先保护以外，显式 `hierarchy_behavior=deneva` 复现 .66 实际 RDMA 插入源码的行为：叶接近满时尝试父节点 CAS 后直接 Abort（含父未满分支的提前 return）；无需分裂时写入后在插入函数内提前释放叶，不等整个事务结束。兼容模式使用原生 RDMA 状态表和真实对象后端，逐次插入即时写回，并记录 early insert/unlock、split abort、已早写后的 abort。它不承诺严格隔离或 abort 回滚，不把源码中不可达的 split 实现声称为已执行；完整 strict 分裂路径另行保留。CSV、配置、续跑和报告按行为/隔离语义分离。

## 实现与检查

- 无全树 topology 事务对象。公共元数据接口返回真实路径，规划 IS/IX/S/X；普通新键插入对叶 X，可能分裂才对最低安全祖先 X；根替换才逻辑 root X。Delete 同时检查子节点余量和不被本事务删除的下界 fence，防止删除后键域路由逃出已持的祖先。
- 整批锁后重规划，并要求当前真实节点及最终模式被已持集合覆盖。计划过期按正常冲突释放重试，不在旧 ID 上继续访问。内存工作区在 Begin 前预留。
- `hierarchy_behavior=deneva` 应用到公共驱动里的每次 Insert：即时写回，随后调用同一个 unlock 方法提前释放真实叶；叶键数达到 Deneva 的 order-2 阈值时尝试父 X，释放父并提前 Abort。父满/未满都走源码当前实际提前返回行为，完整 split 仅 strict 路径执行。A/E 操作混合仍由本项目 trace 决定，不能称为 Deneva MIX_WORKLOAD 原样查询生成器。
- 四个新增 CSV 计数为测量阶段尝试级行为，不以 committed 作为分母：`deneva_early_inserts`、`deneva_early_unlocks`、`deneva_split_aborts`、`deneva_partial_write_aborts`。早写后中止的插入按源码行为保留，不能据 committed 吞吐推断实际净新增行。预热后数据保留，测量计数独立。
- 跨机强制指定 native RDMA + leaf；配置、结果、续跑和报告校验 `hierarchy_behavior` 与 `isolation_semantics`，拒绝把兼容结果放入 strict 组。候选配置 `deneva_source_behavior_2m_10s.json` 只准备好入口，未启动正式矩阵。

硬件 CTest 45/45、零跳过：`experiments/logs/shared-check-20261104-163204-492042/ctest.stdout`（之后仅补父短锁计时，重新完整检查记录另附）。两机五个确定性测试：`experiments/logs/leaf-check-20261104-162734-355713/state.json`，覆盖独立叶插入先完成、真实非根 split、新叶发布后原祖先仍挡住读取、旧路径失效、真实 root 替换、提前释放前 End 的独立读、早写后 abort 留存、源码提前 Abort。另有本地物理删除/整叶摘除/同事务重插后的路由保护回归。

全量定向短检查：strict 的 36 项 `experiments/logs/leaf-check-20261104-162808-144449/state.json`；deneva 的 24 项 `experiments/logs/leaf-check-20261104-162916-470287/state.json`。两套均包含实际 RoCE 三种策略、现有四个子负载、故障和精确和值检查。它们不属于正式性能矩阵。

仍需单独完成两百万规模最大并发容量检查和固定二进制一小时稳定性。索引层的结构批次短排他 latch 仍存在，不能用本次事务屏障消除代替其扩展性结论；普通记录更新已有共享结构 latch 快路径。Payment/New Order 仍是简化 trace，YCSB-E 仍是有限 trace 循环与 upsert，不把这些称为完整 Deneva 业务移植。
