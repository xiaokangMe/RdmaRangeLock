# P3：RDMA WR 批量提交定向验证（2026-11-01）

## 实现

第二阶段把跨多个锁分片的一次锁操作合并为一个 RC QP WR 链：

- `RdmaConnection` 增加 `read_batch`、`write_batch`、`compare_swap_batch`；一批最多 64 个 WR，仅最后一个 WR 置为 signaled，统一轮询一个 CQ completion。
- `RemoteLockManager` 的多分片 gate 获取/释放、active slot 读取、slot 数组读取以及发布写入使用批量接口。
- 单分片路径保留原有直接 read/write/CAS，避免常见 bucket 请求为构造临时请求向量付出额外开销。
- 批量接口在发 WR 前检查空指针、零长度、总 scratch 长度和 64-WR 上限；长度检查先判断单请求大小，避免无符号下溢。
- WR 链发生 post 失败或 CQ 错误时连接进入 failed 状态，不再复用可能仍在飞行中的注册缓冲区。

公共 `IRangeLockManager` 接口、锁槽布局和 next-key `(prev,curr]` 语义均未改变。

## 正确性验证

`build-p3-batch` 上 `ctest --test-dir build-p3-batch --output-on-failure`：9/9 通过。

`rrl_rdma_lock_client` 在 `.66` 的 GID index 3 本机 RoCE 回环上验证了：64-WR 读/写/CAS 链、无效长度拒绝、超过 64 个 WR 拒绝、all-shard gate 中途失败后的已获 gate 回收、next-key 冲突与解锁、全分片锁升级。输出为 `RDMA batch transport and remote next-key lock: OK`；超时和 completion error 均为 0。

## 定向测量

配置文件：`experiments/configs/rdma_p3_wr_batch.json`。每个协议 3 次、每次 10 秒，`.66` 本机 RoCE 回环（GID 3）、YCSB-E、range=8、t32、bucket=64。结果文件：

- `experiments/results/p3-wr-batch-20261101-085451.csv`：6f8c46c 基线与第一版批量路径对照；
- `experiments/results/p3-wr-batch-final2-20261101-114858.csv`：保留单分片快路径后的最终候选样本。

最终候选的平均提交吞吐为 hierarchical **3314.2 txn/s**、next-key **235.1 txn/s**；对应基线样本平均为 **3278.4** 和 **232.0 txn/s**。各样本 `elapsed_ns >= 10s`，`rdma_timeouts=0`、`rdma_completion_errors=0`、`lock_gate_aborts=0`、`lock_capacity_aborts=0`。该测量是 P3 定向验证，不替代正式矩阵，也没有启动跨主机完整矩阵。

`rdma_operations` 统计实际 WR 数，因此批量提交不会减少该计数；本阶段优化的是每批的 post/CQ 轮询和 doorbell 次数。正式矩阵仍应在本里程碑提交后重新生成。
