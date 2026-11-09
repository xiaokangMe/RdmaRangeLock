# Citron 跨主机 RDMA 配置

Citron 的远端对象是段树锁表，索引仍按综述矩阵选择。本项目使用 `.66` workload 客户端、`.67` Citron 被动 RDMA 服务端：服务端只注册段树内存，锁申请通过单边 RDMA CAS/READ/WRITE 完成。

## `.67` 启动服务端

```bash
~/RdmaRangeLock/build-rolex-rdma/rrl_citron_lock_server 28515 1
```

最后一个参数是段树高度。`height=1` 的容量为 256 个 key unit，必须覆盖 workload 的最大 key；更大 workload 应按 `derive_citron_tree_height` 的规则提高高度。

## `.66` 运行 correctness smoke

```bash
~/rrl-rolex-rdma/build-rolex-rdma/rrl_workload_runner \
  --config experiments/configs/citron_rdma_correctness.json \
  --rdma-host 10.102.35.67 \
  --rdma-port 28515 \
  --rdma-lock-only 1 \
  --format csv
```

`--rdma-lock-only 1` 表示使用本地 `v2` 索引、远端 Citron 锁表；这条路径适合单独比较锁协议。它不是远端数据索引路径，若要同时把索引和锁表放到远端，应使用 `index=rdma` 的共享数据配置，并配合原生 RDMA transaction state。

已用 YCSB-A、2 线程、200 事务验证跨主机连接，Citron RDMA session 和 operation counters 均有非零值；不启动正式矩阵。
