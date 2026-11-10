# 本地 YCSB 锁协议矩阵

脚本：`scripts/run_local_matrix.py`

该脚本只启动本机 `rrl_workload_runner`，不启动 RDMA 数据服务、远端锁服务或 TCP 协调器。固定使用 `index=v2`、32 个操作系统线程和 `execution_mode=threads --coroutines 1`，对应当前关闭协程的代码路径。

默认负载和协议：

- YCSB：`ycsb-a`、`ycsb-e`、`ycsb-e-skew`
- 本地协议：`none`、`record-2pl`、`gap-lock`、`next-key`、`prior-key`、`hierarchical`、`exact-range`、`precision-lock`、`griffin-pl`、`citron`
- 等待策略：`no-wait`、`wait-die`、`wound-wait`
- E/E-skew 范围：8、32、128
- 层级锁桶宽：16、64、256
- 测量 10 秒，预热 10 秒，seed=8，单次运行
- A/E 默认 Zipf theta=0.4；E-skew 默认热点数据比例 0.2、热点请求比例 0.8

默认配置对应 224 个 case。`none` 和 `citron` 只运行 no-wait；其他协议运行三种等待策略。`predicate` 不单列，因为当前实现与 `exact-range` 使用同一套区间重叠语义。`blink`、`rolex` 和本地 `index=griffin` 是索引后端/索引变体，不混入锁协议排名矩阵。

运行前先构建：

```bash
cmake -S . -B build-rolex-rdma -DCMAKE_BUILD_TYPE=Release
cmake --build build-rolex-rdma -j8
```

只看计划，不启动负载：

```bash
python3 scripts/run_local_matrix.py --dry-run
```

正式执行本地矩阵：

```bash
python3 scripts/run_local_matrix.py
```

调整 A/E 的偏斜率，仍使用自动分布（A/E 走 Zipf，E-skew 走热点分布）：

```bash
python3 scripts/run_local_matrix.py --zipf-values 0 0.4 0.7 0.9
```

调整 E-skew 热点数据比例和热点请求比例：

```bash
python3 scripts/run_local_matrix.py \
  --hotspot-data-fractions 0.1 0.2 \
  --hotspot-opn-fractions 0.8 0.9
```

两类参数会做笛卡尔积。比如 4 个 Zipf 值、2 个热点数据比例、2 个热点请求比例会生成 16 个 profile；默认 224 个 case 会扩展为 3584 个 case。可以用 `--workloads`、`--protocols`、`--wait-policies`、`--ranges`、`--buckets` 缩小矩阵。

本轮只写脚本、配置和说明，不自动运行负载。结果写入 `experiments/results/`，每个 case 的 stdout/stderr 写入 `experiments/logs/`。
