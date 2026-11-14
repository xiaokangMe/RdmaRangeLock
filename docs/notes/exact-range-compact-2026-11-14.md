# Exact-range 专用压缩 CAS 路径

机器：`.66`，工作树 `/home/dell/RdmaRangeLock-record2pl-20261112`，分支 `codex/record2pl-compact-20261112`。

## 锁表及保护语义

原 exact-range 事务执行器使用通用 `IRangeLockManager`，扫描取得半开区间 S，点读取 S，更新、插入、删除取得点 X。专用实现保留这些读写集合的冲突语义，按请求端点把整个整数键域划成互不重叠的单元：

- 注册每个非空扫描的 start/end；点请求注册 key 和可表示时的 key+1；域末端单独处理 UINT64_MAX，不做溢出的 max+1 运算。
- 单元与 B+ 树中记录的存在性无关。空单元也能持有 S，因此扫描期间的新插入必须与扫描冲突。
- 每个单元使用已有 `RecordLockTable` 的 8 字节原子字：S 为 context 位图，X 为 owner token。共享、排他、wait-die/wound-wait 的身份及年龄校验复用已验证的 authority。
- 一个范围覆盖多个单元时仍需多次 CAS；不能把“每单元 8 字节”解释成“任意范围一次 CAS”或“整个范围只占 8 字节”。不同端点的范围以完全相同的单元身份表示重叠部分，不扩大成桶锁。

整个固定 trace 的几何请求在计时前编译为每事务的有序、唯一资源 ID/模式数组。重复范围合并，同一单元上的 X 覆盖 S。实际执行前按序取得这些锁，持有到写回结束或中止；这改变了相对 generic 的取锁时机，但没有使用冲突结果、事务调度或索引存在性来预判运行。读写与提交仍在运行时执行，遇到冲突仍由所选等待策略处理。

## 省去的热路径操作

不再进入通用锁表的 mutex、已持有区间冲突搜索、动态 holder 分配、全局事务 map、端点查找及运行时合并请求。无需 next-key/prior-key 的物理邻居查找和邻居重检，也无需枚举扫描返回行并逐行加记录锁。每个事务 context 的释放数组按最大计划长度预分配，重试复用。

no-wait 不获取全局年龄票据。wait-die/wound-wait 仍为新逻辑事务获取 age，重试沿用 age；提交检查、伤害处理、超时和写回不可省略。B+ 树结构 latch 及启动/预热 barrier 保留。

`--exact-latency 0 --exact-statistics 0` 编译选择关闭可选直方图、操作/行/checksum 统计；提交、中止、重试、未完成及错误计数保留。fast 的等待直方图和操作计数没有采样，不能把其中的 0 解释成没有等待或没有读写。

## 使用与适用范围

```sh
build-record/rrl_workload_runner --index v2 --cc exact-range \
  --exact-lock-table compact --workload ycsb-e \
  --keys 2097152 --txns 5000 --ops 10 --range 32 --zipf 0.9 --seed 8 \
  --threads 32 --execution-mode threads --wait-policy wait-die \
  --warmup 1 --duration 3 --max-attempts 1000 --wait-timeout-ms 100 --format csv
```

`--exact-lock-table generic` 保留原路径；auto 在本地固定 trace、无外部协调器且 active contexts ≤63 时使用 compact。显式 compact 对不支持的配置报错，auto 回退 generic。63 上限是为保留 S 位图的精确兼容性；没有将第 64 个读者保守映射为 X。普通线程与协程均可用，总 active contexts 采用相同上限。

这是固定请求集合的本地 benchmark 特化。所有访问必须属于预注册 trace；不提供在线接入未知端点的公共锁接口，也不宣称任意动态事务可以直接加入当前 authority。宽范围和高密度端点仍可能产生较长计划。计划内存、释放数组、context 表应与锁字数组分别计量；不能只报 8 字节单元空间。

CSV 增加 `exact_lock_table,exact_lock_bytes,exact_state_bytes,exact_held_capacity_bytes,exact_plan_bytes,exact_latency,exact_statistics`。generic 对应空间值为 0 表示未测量。compact 的 `lock_acquires` 计原子单元请求，与 generic 的区间请求次数含义不同。

## 验证与实验

- `compact_exact_plan_test.cpp`：20,000 组随机事务对的实际 CAS 冲突结果与 `RangeLock::conflicts` 对照，包含重复/重叠请求、S/X 合并、空范围、0 和 UINT64_MAX；检查空隙插入阻塞及半开边界不误阻塞。
- 单线程 generic/compact 混合事务对照包含 scan、insert、update、delete、重插入、读自己写、split 与最大键，比较 checksum、行数、最终键和值。测试预装 seed 的存储保持存活直到最终索引检查完成。
- runner 测试覆盖 YCSB-E、三策略、4/32/63 线程、协程、预热/计时、锁预算中止后清理、自动回退/显式拒绝、metrics-off、直方图及 JSON 配置。
- 22 项相关测试通过，包括现有 record/classic/hierarchy authority、三类 compact runner、通用策略/own-write/scan-batch、B+ 树及配置回归。此检查不是完整形式化可串行化证明。

`scripts/benchmark_compact_exact.py` 顶部 CONFIG 集中控制实验参数。默认 YCSB-E、2^21 keys、32 线程、每事务 10 操作、range 32、seed 8、theta 0/0.9/1.5；三种策略，generic/full、compact/full、compact/fast，各三轮，预热 1 秒/测量 3 秒。短窗口用于优化对照，与此前 10 秒 campaign 不能直接混作同组结果。随机排列 case，轮次交替变体顺序，全部串行执行。保留源文件快照、hash、命令、原始输出和失败记录，报告中位数及 min/max；fast 不能用等待直方图作对比。

有限 trace 在时长窗口中重复执行，预热后 Insert 可能成为已有键覆盖；不将其解释成持续键域增长的原版 YCSB 插入测试。
