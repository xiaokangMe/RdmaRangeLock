# 本地 YCSB-E 的 Deneva Insert 行为

2026-11-17，在 `.66` 的 `codex/record2pl-compact-20261112` 分支实现。

用户确认保留每事务 10 次操作、约 95% 范围读 / 5% Insert 的 YCSB-E 组成；仅对齐 Deneva 的请求固定、间隙小数键、重复插入追加行为。这不是把整个负载切换成 Deneva 的混合事务类型负载。

来源是 `.66:/home/dell/pgm_deneva_backup` 当前工作区：

- `client/client_query.cpp:155`：预生成请求池，取完后计数回绕，重复使用查询。
- `benchmarks/ycsb_query.cpp:385`：Insert 请求保存整数 anchor 加随机四位小数。
- `benchmarks/ycsb_txn.cpp:438`：执行时取固定 decimal_key，运行时新 key 生成调用被注释。
- `system/txn.cpp:5345`：本地叶未满路径找到第一个更大的 key，追加同键条目并增加 num_keys，没有等值拒绝或 upsert。

## 新行为

本地 `rrl_workload_runner --workload ycsb-e --index v2` 默认启用 `--ycsb-insert-behavior deneva`。

1. 每个 Insert 在 trace 生成阶段确定 `anchor + fraction/10000`；fraction 为 `rng % 10000`，允许为零，不强制全局唯一。重试、提交后下一轮重放均保持原 key。
2. 项目键类型仍是 uint64，因此以 `anchor*10000+fraction` 保存同一有序键空间。基础记录 k 编码为 `k*10000`，不再根据 trace 预先挖掉待插入键。
3. 每次提交的 Insert 追加真实叶页条目，即使该 key 已经存在，也增加索引 size；不调用普通 upsert。Update 的原有接口仍保留原语义。
4. 同键条目可以跨多个叶页。闭区间起点必须找到最左侧同键条目，开区间起点跳过所有同键条目；邻接信息返回严格小于 / 大于探测键的 key；锁目录只枚举不同 key。
5. 范围长度仍以原逻辑整数键宽度计量，桶宽 64 仍对应 64 个整数键宽度；编码后宽度分别乘 10000。不是按最终返回行数限制扫描，重复条目会增加扫描工作量。
6. Record 锁表保留每个基础 key 一个 8 字节锁字，通过 stride=10000 定位；小数 Insert key 走稀疏目录，不分配 key_count*10000 个锁字。
7. 事务写集携带 append_duplicate 标记，提交阶段才追加。事务自己的后续扫描可看到自己的待提交新增副本，并同时保留已提交的同键副本。

已有的预热流程不重建索引。预热插入的条目保留到测量窗口；测量期间继续循环追加，因此树规模、扫描行数和分裂成本会随时间增加。这与以前预热后主要重复更新值的工作量不同，不能把新旧曲线直接拼接。

## 使用与可追溯性

```sh
build-record/rrl_workload_runner --index v2 --cc next-key \
  --key-lock-table compact --workload ycsb-e \
  --keys 2097152 --txns 5000 --ops 10 --range 32 \
  --threads 32 --zipf 0 --warmup 10 --duration 10 \
  --wait-policy no-wait --execution-mode threads \
  --ycsb-insert-behavior deneva --format csv
```

JSON 配置对应字段为 `"ycsb_insert_behavior": "deneva"`。结果包含 `ycsb_insert_behavior`、`trace_key_scale`、`index_entries_final`。前述命令是使用示例，本次没有执行完整 10s+10s 七算法偏斜矩阵。

需要重跑历史语义时，显式设置 `--ycsb-insert-behavior legacy-upsert`。底层 WorkloadConfig API 默认仍保留历史语义，以便已有手写整数 trace 和非本地适配器不被隐式重编码；直接调用 API 时设置 `deneva_ycsb_insert=true`。

本次支持共同的本地 v2 底座上的 record、next-key、gap、prior、固定桶层级锁、exact-range、precision 的 compact/generic。其他索引后端，以及 leaf/dag/separator 等结构锁路径，尚未适配重复物理条目，命令行会拒绝该组合。要执行这些既有实验，应显式选择历史语义。

本次没有复制 Deneva 远程叶满路径提前 Abort、父节点满就 Abort、Insert 提前释放锁等其他行为；本地树仍使用现有递归分裂和事务写集提交机制。对齐范围仅是用户确认的 Insert 工作负载行为，不宣称整个执行器与 Deneva 相同。

## 验证

- 新增 C++ 测试：小 order 强制同键条目跨叶/内部节点分裂、开闭边界扫描、严格邻居、4 线程并发重复插入、定点 key 的可重现性与 95/5 比例、完整 seed、稀疏/步长锁表。
- 七协议 × 三策略 × compact/generic 的手写事务两轮提交重放，检查新增行数、事务内读己之写与结构有效性。
- Python runner 测试：42 次有限 trace 的 compact/generic 等价性检查，以及 21 次 4 线程预热 1s + 测量 1s 的重放检查；另检查显式历史 JSON 配置。
- 另做七算法 × 三策略的 32 线程、偏斜率 1.3、16,384 初始键、1s 测量共 21 次并发检查，全部有提交且索引持续增长，索引/锁/释放错误及容量中止均为零。该短检查不作为正式吞吐曲线。
- 已有 14 项 B+ 树、邻接信息、锁表、trace、warmup、配置和各 compact runner 回归检查全部通过，日志保存在 `reports/deneva-insert-20261117`。

旧报告和旧曲线保留。新结果应单独输出，并固定预热时间、测量时间、trace 长度与扫描语义。
