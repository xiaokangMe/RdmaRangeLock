# 桶锁运行时粒度升降（2026-11-07）

## 执行方式

默认两个参数均为0，沿用静态整事务规划；启用任意参数后才逐操作取得父/子锁并维护历史逻辑读写集合，不把静态选择最终模式称为 escalation。

- hierarchy_escalation_threshold / --hierarchy-escalation-threshold：同桶累计不同点记录达到阈值，尝试把现有桶意向原位转换为 S（全读）或 X（含写）。成功发布覆盖后，移除冗余 Record 锁；失败保留旧模式和细锁，继续原操作。
- hierarchy_deescalate_after / --hierarchy-deescalate-after：完成指定数量的操作后，恢复全部历史记录 S/X 和原始扫描的 HierarchyPredicate S（含空结果），再原位把桶覆盖减为 IS/IX。后续操作继续保留历史保护；允许减去额外过锁，绝不减去已经访问的记录和谓词。

de-escalation 是统一一维键底座上的逻辑保护等价转换。底层物理锁条目可替换，所有历史逻辑谓词仍 strict 持有到事务结束；不声称物理锁条目从不变化，也不声称复现某引擎的物理页降级代码。扫描细化使用谓词后代保护，是明确的工程适配，不能和纯物理叶协议混称。

支持固定桶的 local/native RDMA；leaf、Deneva early-release、TCP 镜像协调器均拒绝启用，避免物理模式与第二份授权元数据不一致。现有静态配置默认路径不变。

## 错误与指标

新增 escalation_attempts/escalations/escalation_failures 和对应 deescalation 三项，CSV 同时记录两个阈值。attempts=successes+failures，失败包括因 wound/deadline 在转换完成前取消的尝试，不能将它等同物理冲突数。冲突/gate/capacity 的机会式升级失败不计事务中止；错误/未知返回码停止运行；释放失败是硬错误。

所有历史和 bucket workspace 在工作线程启动前预留；检查容量，不在持锁状态扩展这些向量。降级后的远端缓存必须 exact replace，不能再把旧 X 当满足后续请求。

## 验证

构建成功，真实 RDMA 回环完整 CTest50/50，通过且无跳过。
日志 experiments/logs/classic-mgl-check-20261107-083957/ctest.log。
单线程 A/E × 三种等待策略，对照静态协议的提交数、checksum、返回行数，确认真实升降计数；错误注入覆盖 ERROR/未知码、普通冲突/gate/capacity 保留原细锁。共享远端数据 + native state 的 A/E × 三种等待策略也执行升降并检查计数守恒。底层共同契约验证连续 phantom/记录保护及降级缓存。不运行正式矩阵。

示例：
    build-shared/rrl_workload_runner --cc hierarchical --workload ycsb-a --keys 32 --txns 40 --ops 8 --bucket 32 --threads 2 --wait-policy wait-die --hierarchy-escalation-threshold 2 --hierarchy-deescalate-after 4 --format csv

RDMA 使用相同选项与 rdma-hierarchical、远端 index/data/lock/txn-state 端口；服务须为 range语义5/TCP4 对应代码。正式矩阵脚本支持配置这两个阈值并检查计数，尚未测量收益。
