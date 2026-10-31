# 跨主机 RoCE 验证记录（2026-10-30）

## 拓扑

- 服务端：`.66`，`mlx5_0`，RoCE IPv4 GID index `3`，RDMA 网络地址 `10.0.0.3`。
- 客户端：`.67`，`mlx5_0`，RoCE IPv4 GID index `3`，RDMA 网络地址 `10.0.0.4`。
- 管理网地址为 `10.102.35.66/67`；实验中的 TCP QP 握手和 RDMA 地址均使用 `10.0.0.3/4`。
- 两端网卡均为 `PORT_ACTIVE`、Ethernet link layer，互 ping 正常。

## 验证命令

在 `.66` 启动服务端：

```bash
RRL_RDMA_GID_INDEX=3 build/rrl_rdma_lock_server 19921
```

在 `.67` 运行客户端：

```bash
RRL_RDMA_GID_INDEX=3 build-cross/rrl_rdma_lock_client 10.0.0.3 19920
```

单客户端输出：`RDMA remote next-key lock: OK`。

## 负载结果

`.66` 服务端、`.67` 客户端，客户端使用 8 个 worker/QP，YCSB-E，200 transactions，4 operations/transaction，2000 keys，range=32：

| 模式 | bucket | committed | aborted | lock acquires | lock conflicts |
|---|---:|---:|---:|---:|---:|
| RDMA Next-Key | — | 22 | 178 | 3189 | 178 |
| RDMA Hierarchical | 64 | 25 | 175 | 406 | 175 |
| RDMA Hierarchical | 256 | 25 | 175 | 380 | 175 |

三组命令均返回 0，服务端日志显示每组 workload 建立了 8 条客户端连接；冲突是当前 no-wait 协议的预期 `Abort`，不是连接失败。

## 故障恢复

`.67` 客户端先以 `crash` 参数异常退出，再启动新的 `.67` 客户端重新获取 Next-Key 锁，恢复客户端输出 `RDMA remote next-key lock: OK`。说明服务端断连清理在跨主机 RoCE 路径上生效。

## 结论与边界

跨主机 RC QP、远程读写、CAS、多 QP workload 和断连清理均已验证。正式性能结论仍需固定 workload、重复次数、延迟分位数和跨主机对照实验；本记录只证明跨主机功能路径可用。
