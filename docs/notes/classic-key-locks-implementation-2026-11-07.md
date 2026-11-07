# gap / next-key 修正及 prior-key 实现（2026-11-07）

## 完成范围与依据

按用户要求先修 gap-lock、next-key，再增加 prior-key；本地和 RDMA 共用 src/txn/key_range_plan.h 与同一个事务驱动。旧 run_gap_lock/run_next_key_lock 入口也转入该驱动，保留单次尝试与调用者线程数，不再运行旧的查询端点近似。IRangeLockManager 签名不变。

| 协议 | 本次机制 | 依据 |
|---|---|---|
| gap-lock / rdma-gap-lock | 真实物理相邻键之间开区间 guard；S/X guard 共存；Record S/X 保护记录；InsertIntent X 保护插入位置，受覆盖 guard 排斥；不同位置插入可共存 | MySQL 8.4 §17.7.1 [@innodb84locking] |
| next-key / rdma-next-key | 基本 S/X 单元 (prev,k]；缺失键保护所在完整单元；删除保护自身和后继单元 | Graefe2010 §5.1–5.3 [@graefe2010] |
| prior-key / rdma-prior-key | 镜像基本 S/X 单元 [k,next)；缺失键保护所在完整单元；删除保护自身和前驱单元 | Graefe2010 §5.1 [@graefe2010] |

next/prior 基础版本不包含 ARIES/KVL 的全部 IX/SIX、instant、lock-state replication，不称完整 KVL [@mohan1990kvl]。gap 是选定 InnoDB 规则在统一键区间锁表上的实现，不能称整个 InnoDB 引擎复现。

锁表存储稳定数值区间，后续结构变化不缩小已持逻辑保护；新请求必须与原区间相交检查。因此不用易变物理槽作为唯一锁名，不执行物理 successor 锁名迁移。这是工程适配，冲突可能比原系统保守，检索成本也不同，不能套用原论文吞吐。

## 正确性细节

- 前驱/后继由权威索引的结构元数据取得，无穷由 has_start/has_end 表示，UINT64_MAX 是普通键。
- 先取得邻接键及物理键集合，获取事务锁后重新读取并校验；边界变化导致普通事务中止，候选值从不消费，只消费锁后值。
- ghost key 仍是物理边界。gap 扫描对范围内全部物理键加 Record S，包含不被 live scan 返回的 ghost，防止原位复活绕过开间隙。
- 所有逻辑保护持有到 commit/abort；own-write overlay、三种等待策略共用。
- 锁槽区域语义版本由 2 升为 3；TCP 兼容协议 magic 升为 RRLWAIT2。旧服务/旧客户端须拒绝混用，升级时重启对应服务，不能续接旧服务。没有停止用户现存服务。
- 修复本地 native no-wait 把底层 ERROR 误归类为 Conflict 的错误；不确定取得的锁会纳入清理。单次、fallback batch、原生 batch 均有故障注入。
- 测试交错中的 ABA：若物理键集合恢复且锁后值重读合法，可以提交；真正改变物理键集合必须中止。测试分别保留这两类行为。

## 验证及限制

cmake --build build-shared -j8 成功；真实 RDMA 回环完整 CTest 49/49 通过，无跳过。
最终日志：experiments/logs/classic-mgl-check-20261107-082013/ctest.log。

覆盖真实邻接边界而非 lo/hi、读写两个到达顺序、空树/空扫描、not-found、0/UINT64_MAX、首尾无穷、多键/跨叶、删除相邻单元、边界删除后的原 gap 保护、ghost revival、锁后取值、自身写可见性、硬错误和清理。共享 RDMA 数据 + native 状态的 record/gap/next/prior 四条路径分别执行三种等待策略的小规模正确性负载。

这不是正式矩阵、跨主机性能测量、1小时压力验收或所有可串行历史的证明；没有新性能结论，旧结果仍绑定旧代码，不可重标为本次实现。

## 使用

本地 prior-key：
    build-shared/rrl_workload_runner --index v2 --cc prior-key --workload ycsb-e --keys 128 --txns 24 --ops 2 --range 8 --threads 2 --wait-policy no-wait --format csv

其他策略只替换 --cc 为 gap-lock/next-key；wait-die、wound-wait 通过 --wait-policy 切换。省略显式测量/等待选项的 legacy 入口保持单次尝试；正式比较应统一显式配置运行方式、等待策略与 max-attempts。

RDMA 改用 --index rdma --cc rdma-prior-key，配置同一代码版本启动的数据服务、range lock 服务与 txn-state 服务；传入 --rdma-host、--rdma-port、--data-port、--txn-state-port，容量使用 --rdma-lock-slots-per-shard 匹配服务。容量和吞吐需要后续单独测量，未自动启动矩阵。

## 后续独立里程碑

桶锁完整模式矩阵、运行时粒度升降、DAG 多路径仍按 classic-range-and-full-mgl-plan-2026-11-07.md 推进；本次完成经典范围锁，不把这些事项列作已完成。
