# 正式矩阵首跑发现（2026-10-31）

执行位置：`.66`（`/home/dell/RdmaRangeLock`），commit `b808549`
脚本：`scripts/run_formal_matrix.sh`
文档：`docs/notes/formal-performance-matrix.md`

本文只记录首跑过程中暴露的问题与实测证据，**不含除 B-1 之外的任何代码改动**。B-1 是让脚本能跑起来的最小修复，未提交。

---

## 零、运行环境与执行状态

| 项 | .66（控制端 + 锁服务端） | .67（RDMA 客户端） |
|---|---|---|
| CPU | Xeon Gold 5218 ×2，64 线程 | 64 线程 |
| 内存 | 125 GB | — |
| RoCE | `mlx5_0`，GID 3 = `10.0.0.3`（RoCE v2） | `10.0.0.4` |
| 免密 | `~/.ssh/rdmarangelock_deploy` | 已通 |
| 构建 | `build-formal/`（Release，本次首次完整构建成功） | `/tmp/rrl-formal-build`（脚本自动同步+构建） |

执行方式：`tmux` 会话 `rrl-local`，日志 `experiments/logs/local-matrix.out`，
结果 `experiments/results/formal-local-20261031-024829.csv`。

撰写时状态：**405 / 756 完成，0 失败**，正在 ycsb-e range=128 阶段。

---

## 一、阻断级问题（会让一轮实验白跑）

### B-1 `run_formal_matrix.sh` 的 CSV 校验写反，第一个 case 就中止（已修）

位置：`scripts/run_formal_matrix.sh:155`（原）

```bash
if [[ "$line" != workload,* ]]; then
    echo "invalid CSV output for $case_id: $line" >&2
    return 1
fi
```

`--format csv` 下 runner 先输出表头、再输出数据行，`tail -n 1` 拿到的是数据行
（形如 `ycsb-e,v2,none,1,...`），而校验期待它以 `workload,`（表头）开头。
于是**每个 case 都被判 invalid**，`return 1` 触发 `set -e`，整个矩阵在第一个 case 后退出。

实测现象：

```
invalid CSV output for ycsb-e-none-t1-r1-g32-b0: ycsb-e,v2,none,1,100,100,0,...
```

已改为按 workload 名前缀匹配（7 行 diff，未提交）：

```bash
case "$line" in
    ycsb-* | tpcc-*) ;;
    *) echo "invalid CSV output for $case_id: $line" >&2; return 1 ;;
esac
```

### B-2 单个 case 超时会终止整个矩阵，而不是记失败继续

位置：`run_case` 内 `return 1` + 脚本顶部 `set -euo pipefail`

`run_case` 在 runner 非零退出时 `return 1`；由于它是在 `for` 循环里作为简单命令调用，
`set -e` 会让**整个脚本立刻退出**，后面所有 case 全部丢失。`RRL_TIMEOUT_SECONDS` 默认 900。

实测风险：`ycsb-e / record-2pl / range=128 / t8` 单个 case 已跑 **111 秒且仍在继续**。
按当前趋势，同配置 t16、t32 很可能逼近或超过 900 秒——一旦触发，矩阵会停在 ycsb-e 阶段，
tpcc-payment、tpcc-new-order 全部跑不到。

建议方向：`run_case` 失败只记录到 stderr 并 `continue`，或把整轮拆成可续跑的 per-case 清单。

### B-3 跨主机 RDMA 被锁表容量打满，中止与真实冲突无关

位置：`include/lock/remote_lock_manager.h`（`kRemoteLockSlotsPerShard = 64`）、
`src/lock/remote_lock_manager.cpp:lock()` 中 `if (table.size() == kRemoteLockSlotsPerShard) return Abort;`

诊断方法：固定单线程、单客户端（此时不存在真实锁竞争），只改 range 长度。
同一 trace（seed=8，100 txn，10 ops，keys=10000，zipf=0.99）：

| range | 每事务平均持锁数 | committed | aborted |
|---|---|---|---|
| 1 | 20 | 100 | 0 |
| 4 | 48 | 100 | 0 |
| 16 | 159 | 93 | 7 |
| 32 | 258 | 49 | **51** |

`lock_conflicts` 全程为 0，而 `aborted` 随"单事务持锁数"单调上升——这是**槽位容量上限**，
不是冲突。原因：分片按 `key / bucket_width % 16` 映射，一次范围扫描锁的是连续 key，
天然落在相邻分片；一个事务 9.5 次扫描累积几百把锁，很快把某个分片的 64 个槽写满。

后果：默认矩阵 range 取 8/32/128，后两档基本被容量吃掉，跨主机那 432 个 case
跑完也无法解释"next-key 与 rdma-next-key 的传输层差异"。**本轮已停在跨主机小矩阵，未铺开。**

### B-4 本地 RDMA 回环建链失败，RDMA 只能测跨主机

在同一台机器上用 `127.0.0.1` 或 `10.0.0.3` 连接本机锁服务端，双方都在 QP 建链阶段失败：

```
RDMA lock server listening on port 19932
RDMA lock server accept failed          # 服务端退出
failed to connect RDMA lock server at 10.0.0.3:19932   # 客户端
```

跨主机 `.66 ↔ .67` 则完全正常（同步、构建、建链、回传全通）。
这与矩阵设计一致（`--mode local` 不含 RDMA 协议项），但意味着**任何"同机 RDMA"对照都无法做**，
不能区分"RDMA 慢"与"跨主机链路慢"。若要归因，需要先修好回环建链。

---

## 二、数据效度问题（能跑，但结论会被误读）

### V-1 线程维度整体崩塌，t32 吞吐只有 t1 的 0.7%

ycsb-e，range=8，run 1，`throughput_committed_txn_s`：

| cc | t1 | t2 | t4 | t8 | t16 | t32 | t1→t32 |
|---|---|---|---|---|---|---|---|
| none | 176,310 | 139,411 | 130,516 | 97,919 | 105,654 | 73,896 | 2.4× 下降 |
| record-2pl | 75,799 | 18,478 | 8,486 | 1,563 | 1,134 | 542 | **140× 下降** |
| gap-lock | 53,646 | 10,560 | 4,480 | 1,073 | 798 | 446 | **120× 下降** |
| next-key | 63,782 | 17,670 | 8,051 | 1,586 | 1,078 | 508 | **126× 下降** |
| hierarchical b16 | 153,300 | 70,427 | 57,365 | 42,108 | 9,673 | 5,802 | 26× 下降 |
| hierarchical b64 | 150,677 | 86,408 | 67,588 | 52,754 | 12,548 | 7,213 | 21× 下降 |
| hierarchical b256 | 142,412 | 77,859 | 71,254 | 55,769 | 14,964 | 8,354 | 17× 下降 |

所有协议都是**线程越多越慢**，且带锁协议比无锁基线掉得狠得多。根因是
`LockManager` 只有一把全局 `mutex_`（`include/lock/lock_manager.h:43`），
且每次加锁对 `std::vector<RangeLock>` 做 O(n) 全表扫描——事务持锁越多，临界区越长，
线程数一上去就是二次方退化。锁越多（gap-lock > next-key > record-2pl）掉得越明显，
正好与实测顺序一致。

**结论：本矩阵的 threads 维度目前测的是互斥锁排队时间，不是并发度。**
"v6 在 32 线程比 v5 快"这类结论在当前实现下没有意义——两者都在同一把 mutex 后面排队。

目前唯一可解释的对比维度是 **t1（无线程竞争）的锁开销**：
none 176k > hierarchical 142k~153k > record-2pl 76k > next-key 64k > gap-lock 54k。
这个排序是干净的，可以直接用。

### V-2 YCSB-E 的插入永远落在扫描区间之外，四档 abort 全为 0

位置：`src/txn/workload.cpp` 的 `ycsb()`

```cpp
// 范围读： [k, min(key_count, k + range_length))
// 插入  ： key_count + n * operations_per_transaction + i   （恒 ≥ key_count）
```

插入 key 的下界就是 `key_count`，而扫描区间上界不超过 `key_count`，两者**系统性不相交**。
实测：ycsb-e range=8 t4，none / record-2pl / gap-lock / next-key 四档 **aborted 全部为 0**，
`lock_conflicts` 也是 0。

后果：v3（只锁行）不会产生幻读，v4/v5 防住的幻读本来就不存在。
本轮矩阵在 ycsb-e 上能测"锁开销"，**测不了"幻读防护"**——而这正是这个项目要论证的核心。

### V-3 TPC-C 的 seed 向量吃 1.19 GB，且随事务数线性增长

位置：各处 `seed_n = std::max(key_count, max_key + 1)` + `std::vector<uint64_t> seed(seed_n)`

TPC-C 的 key 按仓库号展开（`u = 2000000 + w*30000 + ...`），`max_key` 到亿级，
但向量按线性数组分配。实测单个 `tpcc-payment`（keys=10000，txns=5000）：

```
Elapsed (wall clock) time : 0:00.61
Maximum resident set size : 1,193,268 KB   ≈ 1.14 GB
```

而真正插入索引的 key 只有几千个。事务数翻倍，内存也翻倍（10000 txns ≈ 2.3 GB）。
125 GB 内存扛得住，但这是纯粹的内存浪费，且 TPC-C 阶段每个 case 都要重来一遍。

---

## 三、本轮确认可用的部分

- **构建与执行链路**：`build-formal/` 一次构建成功；本地 756 case 的调度、日志、CSV 落盘均正常，405 个 case 零失败。
- **结果格式统一**：`formal-local-*.csv` 有完整表头、29 列、跨 case 可直接 `awk`/`pandas` 解析，比之前的三个"假 CSV"是实质进步。
- **committed / attempt 双吞吐口径**已进 runner（`throughput_committed_txn_s` / `throughput_attempt_txn_s`），此前的口径问题已解决。
- **锁数量梯度**（ycsb-e，range=8，t4，三次重复稳定）：

  | cc | lock_acquires |
  |---|---|
  | record-2pl | 382,632 |
  | next-key | 430,152 |
  | gap-lock | 525,192 |

  gap-lock 比 next-key 多约 22%，与"gap lock 需要额外锁住区间两端"的预期一致。
- **跨主机 RDMA 通路**：同步 → 远程构建 → RoCE 建链 → 结果回传全链路可用，只是受 B-3 限制暂不能出性能结论。

---

## 四、建议的处理顺序

| 顺序 | 事项 | 理由 |
|---|---|---|
| 1 | B-2：单 case 失败改为记录并继续 | 不改则 range=128 一旦超时，后续 350 个 case 全部丢失 |
| 2 | B-3：`kRemoteLockSlotsPerShard` 64 → 512（或按事务最大持锁数估算） | 不改则跨主机 432 个 case 无意义；代价是 region 从 64 KB 变 512 KB，读放大上升 |
| 3 | V-2：把 YCSB-E 的插入挪进已扫描区间 | 不改则整个项目的核心命题（幻读防护）无法测量 |
| 4 | V-1：锁表去掉全局 mutex 后再跑 threads 维度 | 否则所有多线程数字只能作废；短期可先把 threads 维度标注为"仅测互斥开销" |
| 5 | V-3：TPC-C seed 改为稠密映射或按需分配 | 纯浪费，但 125 GB 内存暂时不会炸 |
| 6 | B-4：修本地 RDMA 回环 | 只影响做同机对照，不影响跨主机结论 |

---

## 五、复现命令

```bash
cd /home/dell/RdmaRangeLock

# 本地全矩阵（本次执行中）
tmux new -s rrl-local "scripts/run_formal_matrix.sh --mode local > experiments/logs/local-matrix.out 2>&1"

# 跨主机小矩阵（受 B-3 限制，只在 range ≤ 4 时数据干净）
RRL_SYNC_REMOTE=0 RRL_RUNS=1 RRL_THREADS=1,4 \
RRL_WORKLOADS=ycsb-e RRL_RANGE_LENGTHS=1,4 RRL_BUCKETS=64 \
RRL_TRANSACTIONS=100 scripts/run_formal_matrix.sh --mode cross-host

# 单点诊断：range 与每事务持锁数的关系
./build-formal/rrl_rdma_lock_server 19930 &
RRL_RDMA_GID_INDEX=3 ./build-formal/rrl_workload_runner --index v2 --cc rdma-next-key \
  --workload ycsb-e --keys 10000 --txns 100 --ops 10 --range 32 --threads 1 \
  --seed 8 --zipf 0.99 --format csv --rdma-host 10.0.0.3 --rdma-port 19930
```

---

## 六、与 `code-review-2026-10-30.md` 的关系

昨日那份清单里的条目在本轮大部分**依然存在且已实测确认**：
`build-rdma` 缺失已由 `build-formal` 取代（P0-17 关闭）；
吞吐口径已修（P1-13 关闭）；其余（全局 mutex、YCSB-E 插入位置、TPC-C 内存、
no-wait 语义、v5 `rows_scanned` 翻倍）本轮都拿到了实测证据，见 V-1 / V-2 / V-3。

新增的只有 B-1~B-4 四条，都是"跑起来"才暴露的问题。
