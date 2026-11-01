# 跨主机 1 秒卡顿：313 txn/s 不是性能，是超时（2026-11-01）

## 起因

正式矩阵报告里有一个"形状不合常理"的点：跨主机 `rdma-hierarchical`、bucket=64、t16 的
提交吞吐只有 **313 txn/s**，而相邻的 t8 是 2,364、t32 是 1,965。单独复测后确认：
**这个数字不是性能劣化，而是一次固定 1 秒的超时把 elapsed 拉长了。**

## 证据

### 1. 313 = 5000 / 1.00s

吞吐 = committed / elapsed。该配置 committed 恒为 313（5000 / 16，每线程一个事务），
如果 elapsed 是 1.00 秒，吞吐就正好 313。

用 `transactions / throughput_attempt_txn_s` 反推 elapsed，32 个 case 中：

| elapsed | 正常 | 1.00s |
|---|---|---|
| t8 | 0.26s（18,912 attempted/s） | 1.0013s（4,993 attempted/s） |
| t16 | 0.14s（35,725 attempted/s） | 1.0011s（4,994 attempted/s） |

`attempted tput ≈ 4,990` 这个数本身就是 5000/1.00s，是"整整跑了一秒"的指纹。

### 2. 整份跨主机矩阵里 8.3% 的 case 有同样指纹

432 个 case 中 **36 个**（8.3%）的 elapsed 落在 0.98~1.02 秒：

| 线程数 | 2 | 4 | 8 | 16 | 32 |
|---|---:|---:|---:|---:|---:|
| 命中数 | 18 | 8 | 2 | 5 | 3 |

**分散在所有线程数、两个协议、四个 workload 上**，不是 t16 或 b64 的专属现象。
t16 b64 只是运气差，三次重复里有两次命中，中位数被拉到了 313。

### 3. 聚焦复测：8/8 命中

固定 `rdma-hierarchical / b64 / ycsb-e / range=8 / t16`，跑 8 次（seed 8~15）：

```text
run  committed  aborted  commit_tput  attempt_tput  elapsed   stall
 1        313     4687        312.6          4994   1.0011   YES
 2        313     4687        312.5          4992   1.0017   YES
 ...
 8        313     4687        312.7          4995   1.0010   YES
```

八次全部命中，elapsed 稳定在 1.0010~1.0020 秒。换一个全新启动的服务端再跑三次：
前两次命中、第三次正常（0.14s）。说明**不是确定性的配置问题，而是随环境变化的偶发事件**，
只是那一段时间里命中率很高。

### 4. 排除链路问题

```text
port_rcv_errors   = 0
port_xmit_discards= 0
rx_out_of_buffer  = 0
tx_errors_phy     = 0
state             = 4: ACTIVE
```

链路干净，所以是软件路径上的超时，不是物理层丢包。

## 判断

时间精确落在 **1.00 秒**，与 `src/rdma/rdma_transport.cpp` 里 CQ 轮询的 1 秒 deadline 完全吻合
（`docs/notes/code-review-fixes-2026-10-31.md` 记录过这条改动）。也就是说：
某次 RDMA 操作的完成事件没有回来，`post_and_poll` 空转了整整一秒才放弃。

具体是哪一次操作、为什么丢完成事件，目前没有证据链——需要加计数器才能定位，
这一点不要凭猜测写进文档。

## 影响

1. **单次测量可能偏 5~20 倍。** 这个 workload 正常 elapsed 是 0.14~0.26 秒，
   一次 1 秒卡顿直接把吞吐压到 1/7 ~ 1/20。三次重复取中位数**不足以**抵抗这种污染
   （t16 b64 就是 2/3 命中，中位数全废）。
2. **跨主机那半张表的所有吞吐数字都要打折看。** 8.3% 的 case 受影响，随机分布，
   无法从外部识别哪些被污染——除非用 elapsed 反推。
3. 本地矩阵不受影响（无 RDMA 路径）。

## 建议

1. **先加可观测性，再谈性能**：runner 输出 `elapsed_ns` 与一个 `rdma_timeouts` 计数器
   （`post_and_poll` 命中 deadline 时自增）。现在这个事件完全静默，只能靠 elapsd 反推。
2. **判定规则**：任何 `rdma_timeouts > 0` 的 case 标记为无效并重跑，不要进统计。
3. **短期**：跨主机结果里凡是 `attempted tput ≈ 5000`（或 elapsed ≈ 1.00s）的行先剔除，
   或在报告里显式标注。本轮 36 个这样的 case，剔除后 t16 b64 的中位数应回到 2,000 以上。
4. **不要**把 313 当成"t16 掉点"写进任何结论——它与 bucket 宽度、线程数、协议都无关。

## 复现

```bash
# 服务端
cd /home/dell/RdmaRangeLock
RRL_RDMA_GID_INDEX=3 ./build-formal/rrl_rdma_lock_server 19955 &

# 客户端（.67）
RRL_RDMA_GID_INDEX=3 /tmp/rrl-formal-build/rrl_workload_runner \
  --index v2 --cc rdma-hierarchical --workload ycsb-e --keys 10000 --txns 5000 \
  --ops 10 --range 8 --bucket 64 --threads 16 --seed 8 --zipf 0.99 \
  --rdma-host 10.0.0.3 --rdma-port 19955 --format csv
```

判读：`transactions / throughput_attempt_txn_s` 若 ≈ 1.00，即为命中卡顿。
