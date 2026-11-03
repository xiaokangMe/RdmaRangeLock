# 等待策略、延迟分布与谓词锁实现方案

## 范围

本里程碑先做：NO_WAIT / Wait-Die / Wound-Wait 可切换、事务级重试与等待指标、p50/p95/p99/p999 延迟、Predicate Lock。B-link、乐观下降、新负载不在本里程碑。

`IRangeLockManager::lock/unlock/unlock_all/get_lock_count` 的既有签名保持不变。策略选择作为管理器初始化配置，不能把事务策略隐式塞进 LockKind。默认 NO_WAIT，旧命令与旧 CSV 行为保持向后兼容。

## 事务与策略语义

- 每个逻辑事务有固定开始时间戳；每次重试使用新的 attempt id，但继承逻辑事务年龄。事务结束后释放严格 2PL 锁。
- NO_WAIT：遇冲突立即 abort 当前 attempt；驱动重新执行同一逻辑事务，计入 retry 和端到端延迟。
- Wait-Die：请求者比所有冲突持有者都老才排队等待；否则当前 attempt abort 并重试。解锁通过条件变量通知等待者，不轮询、不 sleep。
- Wound-Wait：较老请求者标记较年轻持有者为 wounded，并在等待队列中等待其回滚释放；较年轻请求者等待较老持有者。持有者在每个操作边界和提交前检查 wound 标记，禁止提交已被撤销的事务。
- 同一锁资源、锁升级和多资源持有都必须遵循事务年龄规则；condition-variable 虚假唤醒后重新检查锁表。升级等待须验证避免同事务自等待。
- RDMA 策略不能只在客户端重复读写表。服务端须原子维护事务年龄、等待队列、wound 标记，并提供可唤醒的客户端通知；连接断开/租约失效后的 waiter 清理也要有界。控制面与共享 slot 表修改必须有明确的原子顺序和崩溃恢复语义。完成该服务端协议前，RDMA 运行器须拒绝 Wait-Die/Wound-Wait 配置，不得静默退回 NO_WAIT。

## 指标

- Throughput：committed logical transactions / measured second。
- Latency：分别输出成功 attempt latency 与 logical completion latency（首个 attempt 开始至最终提交），后者包含所有等待、abort 和重试。
- 分位数：每个独立 run 计算 p50/p95/p99/p999，并报告样本量。跨 run 汇总中位数和 min-max；不把多 run 样本混池，也不称 min-max 为置信区间。p999 样本量不足必须标注不稳定。
- Retry：attempts / committed transaction、retries / committed transaction、conflict aborts、max retry streak。限制最大重试/总时限时，未完成逻辑事务单列，不得从延迟样本中消失。
- Wait：锁队列等待总时长与分位数；RDMA transport/QP/gate 等待独立统计，不能混称事务锁等待。
- Abort rate 保留为辅助指标。NO_WAIT 的 lock wait 应为零附近，但 retries/logical latency 仍必须纳入比较。
- 使用有界预分配样本存储或固定内存 histogram，不在持事务锁时分配/输出/落盘。CSV 列版本化；诊断输出与正式结果通道分离。

## Predicate Lock

`LockKind::Predicate` 采用独立驱动：范围读对完整逻辑谓词取得 S，写/插入/删除对影响键取得 X；谓词 overlap 决定冲突。扫描前后验证候选集合，避免 scan 与锁安装之间的幻读窗口。与 Record/Gap/NextKey 模式分开配置，加入空结果、边界、插入、删除、跨 predicate、并发与释放测试。本项目目前的区间 LockKind 已提供 overlap 基础，但不代表 predicate 驱动与验证协议已完成。

## 分阶段验收

1. 先在本地锁管理器实现无轮询 wait queue 和三策略确定性语义测试；包括老/年轻事务、交错两锁死锁场景、wound 回滚、升级、超时/取消和释放唤醒。
2. 驱动以逻辑事务为单位重放，记录 attempt 与 logical latency，并用独立测试校验计数守恒、分位数和事务身份/年龄稳定。
3. RDMA 控制面协议及真实双机验证通过前，RDMA 明确只接受 NO_WAIT。服务端崩溃恢复、客户端断线和租约清理属于正确性测试，不可略过。
4. 增加 Predicate Lock 驱动并验证幻读防止，再跑相同协议的小型正确性和定向 smoke；正式矩阵由单独授权决定。

本方案本身不引入 Wait-Die/Wound-Wait 的性能主张，不运行正式矩阵。
