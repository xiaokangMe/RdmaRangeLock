# 统一 workload trace 接入

## deneva 对照

deneva 的 YCSB 请求使用 `RD`、`WR` 和连续范围查询三类访问；生成器支持热点/Zipf 访问分布。TPC-C 的 Payment 和 New-Order 由固定的 warehouse、district、customer、item、stock key 组合组成。轻量项目保留这些访问语义，去掉 RDMA、row_t、远端节点和事务调度依赖。

## 统一表示

`include/txn/workload.h` 定义：

- `WorkloadConfig`：key 数、事务数、每事务操作数、范围长度、线程数、Zipf 参数和随机种子；
- `Operation`：Read、Update、Insert、Delete、RangeRead；
- `TransactionTrace`：事务 ID 和有序操作列表；
- `WorkloadTrace`：YCSB-A/B/E 与 TPC-C Payment/New-Order-lite 生成器；
- `RunStats`：事务数、操作数、范围扫描行数、checksum 和吞吐。

`run_workload()` 对 v1 使用单线程，对 v2 使用多线程，所有版本消费同一份 trace。TPC-C-lite 只表示访问模式，不声称实现完整 TPC-C 事务语义；完整事务约束会在 v3 Strict 2PL 接入后补上。

## 运行

```text
rrl_workload_runner --index v2 --workload ycsb-a \
  --keys 10000 --txns 5000 --ops 10 --threads 4 --zipf 0.99

rrl_workload_runner --index v2 --workload ycsb-e \
  --keys 10000 --txns 5000 --ops 10 --range 20 --threads 4 --zipf 0.99

rrl_workload_runner --index v2 --workload tpcc-new-order \
  --keys 100 --txns 1000 --items 5 --threads 4
```

对应配置为 `experiments/configs/workload_smoke.json`。运行输出是单次 smoke 结果，不能直接作为论文性能结论；正式实验需要按仓库约束重复三次并保存原始数据。