# 高偏斜重试耗尽：原因与自适应退避

工作树：`.66 /home/dell/RdmaRangeLock-record2pl-20261112`；分支：
`codex/record2pl-compact-20261112`。

## 原因

此前专用 CAS executor 在每次失败后释放锁，仅调用一次
`cooperative_yield()` 就开始下一次尝试。OS 线程路径中的 yield 不能保证
锁持有者先获得足够 CPU 完成事务；它也没有给失败者不同的重入时间。
压缩锁表减少了单次尝试成本，因此热点处可能更快用完 1000 次机会。

YCSB-E 每事务十操作，高偏斜下不同事务的扫描和写入会反复覆盖相同键。
Record、next-key、gap、prior 和 Precision 还可能先登记共享保护、随后
升级/申请写保护。no-wait 在冲突处返回；wait-die 的年轻事务中止；
wound-wait 的受伤事务中止。若受害者释放锁后立即重入，就会干扰还没
完成的获胜者。预规划的 hierarchy/exact-range 虽减少了升级和发现开销，
仍有失败者反复争抢同一热点的问题。

这是热点上的调度与进展问题；不能把 CAS 更快等同于吞吐更高，也不能
用增大 max_attempts 来证明问题消失。

## 改动

新增 context-local `RetryBackoff`，接入两个专用 executor，覆盖 record、
next-key、gap、prior、hierarchy、exact-range、Precision。

- 只有一次 attempt 失败并释放全部锁后才运行。
- 前两次失败执行短的随机 PAUSE；层级桶锁 no-wait 的锁冲突通常更短，
  为其保留六次逐步增长的短暂停顿，再进入休眠阶段。
- 后续连续失败使用指数增长、full jitter 的休眠，通常的随机上限为 1024 μs；连续失败达到 32 次后放宽到 16384 μs。
  该上限约束请求的休眠时间，OS 调度可能让实际耗时更长。
- 成功逻辑事务的常规数据路径不调用退避；下一个逻辑事务重新从短暂
  碰撞阶段开始。随机状态为每 context 私有，不增加全局随机数锁/计数器。
- 协程路径仅 cooperative yield，不休眠承载其他协程的 OS worker。
- capacity/budget 配置失败不做退避，它不是等待别人的锁释放能解决的。
- 全局测量窗口截止后不再等待；有限 attempt 上限、事务 age 跨重试保留、
  锁模式兼容关系、伤害/提交检查和私有写集发布规则都保持原语义。

no-wait 的锁请求仍然立即失败；改变的是事务在失败后的重试准入时间。
没有增加全局串行锁、集中事务队列或预先决定谁获胜。

## 配置与测量

```
--retry-backoff adaptive  # compact 默认
--retry-backoff off       # 原来的立即重试行为，用于消融对照
```

JSON 使用 `retry_backoff`，值为 `adaptive` 或 `off`。该设置当前只应用于
上述 compact executor；generic/RDMA/Griffin 输出 `not-applicable`，不冒充
已接入。直接 executor API 同样检查配置值。

CSV 追加字段：

- `retry_backoff`：本次实际选择。
- `retry_backoff_count`、`retry_backoff_total_ns`：实际发生的退避及实测总耗时。
  即使关闭可选 histogram/statistics 也保留这两个计数。
- `committed_read_only`、`committed_with_writes`：按原逻辑事务是否包含
  Update/Insert/Delete 分类，compact 下二者之和必须等于 committed。
  它们不是所有尝试中 read/insert 操作的计数。非 compact 下暂未采集。

逻辑事务完成延迟从第一次尝试之前开始计算，包含中间的重试和退避。
`lock_wait_total_ns` 只表示实际持锁协议的等待，不包含退避；两者不能混淆。
窗口结束还未完成的事务仍计入 logical_unfinished，1000 次耗尽仍如实计入
retry_exhausted，没有把等待藏成提交或减少统计分母。

`scripts/benchmark_retry_control.py` 顶部 CONFIG 可直接编辑。正式对照：
2^21 keys、32 线程、YCSB-E、5000 条 trace、10 操作/事务、range=32、
seed=8、theta=0/1.5、三种等待策略、七种 compact 算法、off/adaptive、
三轮；每项预热 1 秒、测量 2 秒，依次运行，轮次交替新旧顺序。
这是短窗口的性能诊断，保留每次原始值和范围，不冒充长期服务测试。

报告必须同时列提交吞吐、重试/提交、重试耗尽、含写事务提交和未完成数。
原来的限次丢弃可能跳过困难的写事务，使只读事务主导总 TPS；改变完成
比例时不能只用更大的总 TPS 宣称更好的进展。新旧方案在单次测量中也
可能出现吞吐与完成率的取舍。

随机退避是缓解活锁和重试风暴的实现，不是任意负载下有限次必提交的
形式化保证。真正的冲突和有限窗口结束仍然存在。

## 验证

新增 YCSB-E 集成回归覆盖七算法、三策略、off/adaptive 单线程数据结果
一致性、并发计数恒等式、max_attempts=1 的零重试/零退避、协程进展、
预热与测量窗口以及 JSON 配置。现有锁表竞争、幻读边界、读己之写和
准备提交检查继续运行。Release 全 CTest 75 项，54 项实际执行通过，
21 项因缺少 RDMA 环境跳过，零失败。

## 对照结果与进一步优化

初版退避（最大随机窗口 1024 μs）的 252 次诊断已完成，执行错误为零。
theta=1.5 的全部轮次合计：立即重试耗尽 181,984 个逻辑事务，自适应退避
耗尽 3 个。逐轮数据见 reports/retry-ycsbe-20261114T143228Z；中位数为零
并不意味着每轮都是零，summary 保留了合计数和原始日志。

发现少数长期竞争者仍会饥饿，因此后续仅在连续失败至少 32 次时扩大
随机退避窗口。没有增加 max_attempts；测量截止仍会结束该逻辑事务。

Next-key/Prior-key 的 compact 范围锁已经是连续区间的并集，其覆盖只由
首尾物理邻居决定。原来仍在加锁前后各枚举一遍所有内部键，是冗余工作。
现在这两条路径只采集并重检边界，再在完整 S 区间锁保护下扫描实际数据；
原子目录覆盖区间内所有已登记坐标，内部键变化不缩小锁覆盖。边界变化
仍中止重规划。Gap 的计划依赖物理记录集合，继续保留完整枚举及验证。
新增回归使用拒绝内部键枚举的索引包装器，覆盖 sparse insert/delete、
空范围、分裂和读己之写，并与 generic 的单线程数据结果对照。

正式完整矩阵由 scripts/benchmark_optimized_ycsbe.py 顶部 CONFIG 管理，
六算法、九 theta、三策略、每配置一次、预热 10 秒、测量 10 秒。
为衡量实际热路径性能，统一关闭可选延迟直方图和行统计；关键提交/中止/
重试/未完成/退避计数继续采集。绘图程序固定全部真实 theta 刻度。
