# 远端 RDMA 学习索引路径

本项目保留三条可以使用同一 workload driver 的数据路径：

1. **本地索引**：`v2`、`blink`、本地 `rolex` 等，索引页和记录都在客户端进程内。
2. **共享 RDMA B+树**：`index=rdma`，服务端保存 offset B+树页和记录，客户端通过单边 RDMA READ/WRITE/CAS 访问。
3. **远端 RDMA ROLEX**：`index=rolex-rdma`，服务端保存 ROLEX 目录、叶节点和记录，客户端缓存模型提示并通过单边 RDMA READ/WRITE/CAS/FAA 完成查找、范围扫描和插入。

ROLEX 服务端只在启动时根据 workload seed 构造远端内存并注册 MR；数据操作不经过服务端 RPC。TCP 连接只用于现有 `RdmaConnection` 的 QP/MR 握手，数据路径仍是一侧 verbs 操作。叶节点使用 64 个键值槽，版本字段实现 seqlock，目录锁用于分裂时的目录更新。

## 有界正确性 smoke

在 `.67` 启动服务端（配置中的 workload、seed、key_count 必须与客户端一致）：

```bash
./build/rrl_rolex_rdma_server \
  --config experiments/configs/rolex_rdma_correctness.json \
  --port 19879
```

在 `.66` 运行客户端：

```bash
./build/rrl_workload_runner \
  --config experiments/configs/rolex_rdma_correctness.json \
  --rdma-host 10.102.35.67
```

先用 YCSB-A 验证点读写，再用 YCSB-E 验证范围扫描和插入；两者都固定线程模式、关闭协程。该配置只用于 correctness smoke，不作为正式性能矩阵。

当前远端 ROLEX 不包含论文中的后台 retraining/shadow redirection，因此结果应标记为“ROLEX RDMA 适配路径”，不能直接宣称完整论文复现。
