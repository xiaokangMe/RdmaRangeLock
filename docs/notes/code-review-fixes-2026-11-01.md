# 代码审阅修复记录（2026-11-01）

本轮针对正式矩阵报告中的数据有效性问题完成以下修复：

- YCSB-E 的插入 key 改为从扫描区间内的候选位置生成，并从初始索引中排除这些候选 key；范围扫描记录上界，确保 trace 的 key 域和初始化一致。这样记录锁、Gap Lock、Next-Key Lock 会在同一 trace 上产生可解释的范围冲突。
- Next-Key Lock 在扫描下界额外保留一个 point lock，覆盖空的前导 gap；相邻记录仍使用 `(prev,curr]`，尾部 gap 继续覆盖到范围上界。
- 本地锁表按 16 个逻辑 shard 分片，跨 shard 按升序加锁、逆序释放；锁快照和计数去重 shard 副本，避免全局 mutex 串行化。
- RDMA 锁表每片扩展为 256 个槽，并将 `AbortConflict`、`AbortCapacity`、`AbortGate` 分开统计，CSV 与文本输出都保留三类计数。
- TPC-C 初始化从按最大 key 分配的稠密 vector 改为按需 unordered_map，避免 warehouse/order key 展开造成 GB 级空洞内存。
- 正式矩阵脚本会在单 case 失败或超时时记录失败清单并继续，结束时汇总；CSV 表头同步新增容量和 gate 字段。
- workload runner 支持 `--config`，并允许命令行覆盖配置文件字段。

验证：`cmake --build /tmp/rrl-check -j2`、`ctest --test-dir /tmp/rrl-check --output-on-failure`，9/9 测试通过；配置文件和小规模 YCSB-E runner 均已执行。

仍待单独处理：BTreeLatch 的 writer/root 锁扩展性、跨主机完整矩阵重跑，以及本地 RDMA 回环建链。
