# 正式矩阵（本地 + 跨主机）跑后的遗留问题（2026-11-01）

运行：`a2c68f8` + 本次新增脚本，`.66` 控制，`.67` 作 RDMA 客户端
结果：**1188 个 case 全部完成，0 失败**。本地 756 个（30 分钟），跨主机 432 个（25 分钟）。
图表：`figures/formal-both/`；脚本 `scripts/analyze_formal_both.py`、`scripts/plot_formal_both.py`。

---

## 零、这一轮确认修好的三件事

| 项 | 证据 |
|---|---|
| **锁槽容量** | 跨主机 216 万事务，`lock_capacity_aborts` **合计为 0**。10-31 那轮"range≥16 就大片中止"的容量问题彻底消失 |
| **幻读路径** | 本地 YCSB-E 在 t32 出现 **29.2%~36.8%** 真冲突（上一轮四个协议全是 0）。插入 key 落进扫描区间后，记录锁/Gap/Next-Key 的差异第一次真的被压出来了 |
| **本地矩阵速度** | 同样 756 case，上一轮 2h45m，本轮 **30 分钟**。锁表分片生效 |

---

## 一、跨主机：每线程只能提交一个事务（最严重）

`rdma-next-key` 与 `rdma-hierarchical` 的**中止率都恰好是 67.2%**，而且逐线程看完全等于 `1 − 1/threads`：

| threads | committed | aborted | abort% | 理论 1−1/T |
|---:|---:|---:|---:|---:|
| 1 | 5000 | 0 | 0% | 0% |
| 2 | — | — | 50% | 50% |
| 4 | — | — | 75% | 75% |
| 8 | — | — | 87% | 87.5% |
| 16 | — | — | 93% | 93.75% |
| 32 | — | — | 96% | 96.875% |

`committed = transactions / threads`，即**每个线程只有第一个事务提交成功，之后全部中止**。
这条曲线与 workload、range、zipf 全部无关，说明问题在协议实现，不在负载。

### 两个协议的中止来源完全不同

| cc | 事务数 | 中止数 | 中止率 | conflicts | capacity | gate |
|---|---:|---:|---:|---:|---:|---:|
| `rdma-next-key` | 540,000 | 362,828 | 67.2% | **362,828** | 0 | 0 |
| `rdma-hierarchical` | 1,620,000 | 1,088,370 | 67.2% | 0 | 0 | **1,088,370** |

- **next-key 全部记成 conflicts**：与 `rdma-gate-contention-2026-11-01.md` 的推断一致——
  `unlock_all` 抢不到全部 16 个分片 gate 就整体放弃，事务的锁静默残留；
  下一个事务撞上这些残留锁，被判为冲突。
- **hierarchical 全部记成 gate**：v6 的根意向锁是 `unbounded()` 区间，
  `shard_ids()` 对无界区间直接返回全部 16 片，于是每把根锁都要一次抢齐 16 个门，
  t≥2 必然失败。

两者是同一个病灶的两种表现：**全分片 gate 的 all-or-nothing 获取**。

### 已查明：313 txn/s 是 1 秒超时，不是性能劣化

`fig09` 里 `hierarchical b64` 在 t16 的 313 txn/s，经复测确认为 **一次固定 1 秒的 RDMA 超时**
把 elapsed 拉长所致：`5000 事务 / 1.001s ≈ 4,990 attempted txn/s`，吞吐 = committed/elapsed = 313。

- 全矩阵 432 个 case 里有 **36 个（8.3%）** 命中这个指纹，散布在 t2/t4/t8/t16/t32 与两个协议上，
  与 bucket 宽度、线程数、协议均无关；
- t16 b64 只是运气差（三次重复里两次命中），8 次定点击复测 **8/8 命中**；
- 链路计数全为 0（`port_rcv_errors`、`xmit_discards`、`out_of_buffer`），排除物理层。

详见 `docs/notes/rdma-one-second-stall-2026-11-01.md`。

**结论：313 不能当性能数字用，但这些行也不能从数据里剔掉**——36 次卡顿集中在 16 个配置上，
其中 9 个配置三次全中，剔掉等于系统性删掉最慢的那批配置。正确做法是把单 case 事务数
从 5000 提到 50,000~100,000（或改固定时长计时），让一次 1 秒卡顿最多影响 10%，
然后连同超时次数一起原样报告。

---

## 二、本地：分片了，但分片本身成了新瓶颈

### 2.1 `unlock_all` 每次事务结束都拿全部 16 片锁

`src/lock/lock_manager.cpp:111`

```cpp
RC LockManager::unlock_all(txnid_t txn_id) {
    const auto ids = all_shards();                       // 16 片全部
    std::vector<std::unique_lock<std::mutex>> guards;
    for (const auto id : ids) guards.emplace_back(shards_[id].mutex);   // 一次性全拿
    ...
```

`lock()` 只按区间取 1~3 片，但**解锁要一次性拿满 16 片**。事务越短（10 ops）、线程越多，
这个"结尾全局屏障"占比越高：

| cc（YCSB-E range=8） | t1 | t32 | 衰减 | t32 中止率 |
|---|---:|---:|---:|---:|
| none | 175,453 | 160,004 | 1.1× | 0% |
| record-2pl | 55,456 | 1,330 | **42×** | 29.2% |
| gap-lock | 38,879 | 697 | **56×** | 30.1% |
| next-key | 51,401 | 1,255 | **41×** | 30.0% |
| hierarchical | 127,792 | 12,102 | 11× | 36.8% |

`none` 几乎平（1.1×），说明索引层已经不再是主瓶颈；**带锁协议的 40~56 倍衰减里，
只有约 30% 来自中止，其余来自这把全局解锁锁**。

### 2.2 无界区间把所有锁变成全局锁，v6 在写负载上最惨

`shard_ids(range)` 对无界区间返回全部 16 片。v6 的根意向锁正是 `unbounded()`，
所以 **v6 的每把根锁都要锁住整张表**。YCSB-A（50% 更新，热点集中）上直接崩掉：

| cc（YCSB-A） | t1 | t32 | 衰减 |
|---|---:|---:|---:|
| none | 295,496 | 55,233 | 5.4× |
| next-key | 199,639 | 6,222 | 32× |
| record-2pl | 162,980 | 4,790 | 34× |
| gap-lock | 165,609 | 4,622 | 36× |
| hierarchical | 161,450 | **489** | **330×** |

YCSB-E 上 hierarchical 是最快的（127k → 12k），YCSB-A 上是最慢的（161k → 489）。
**同一份实现，两个负载下排序完全相反**——原因就是根锁的全分片获取在写冲突多的场景下退化得最狠。
这一条直接决定 v6 那一节怎么写，不能只引一条曲线。

---

## 三、跨主机的绝对性能不可用于结论

同一 workload、同一协议、t=1：

| 位置 | cc | committed txn/s |
|---|---|---:|
| 本地 | next-key | 51,401 |
| 跨主机 | rdma-next-key | **514** |
| 本地 | hierarchical b64 | 127,792 |
| 跨主机 | rdma-hierarchical b64 | **2,469** |

差 **100 倍**。t=1 时没有并发、没有中止，所以这 100 倍是**纯传输 + gate 往返开销**：
每次加锁/解锁都要 RDMA CAS + 读槽 + 写槽，一次范围扫描要几十把锁。

也就是说：跨主机这半张表现在**只能证明"通路能跑、语义没错"**，
任何"RDMA 比本地慢 N 倍"的结论都必须先扣掉这个实现层面的往返成本，
否则测的是协议实现的往返次数，不是 RDMA 的能力。

---

## 四、优先级

| 顺序 | 事项 | 理由 |
|---|---|---|
| 1 | 跨主机 `unlock_all` 改逐片处理 | 一处改动同时消掉 next-key 的假冲突；改完重跑，abort 率应回落到与热点相关的水平 |
| 2 | 无界区间的分片映射 | 消掉 v6 的 gate 全灭；也顺带缓解本地的 2.2 |
| 3 | 本地 `unlock_all` 改为按事务实际涉及的片解锁 | 直接决定本地四协议的多线程曲线是否可用 |
| 4 | 复核 `hierarchical b64 t16` 的掉点 | 复测三次，确认是否真实 |
| 5 | 把 `unlock_all` 的失败计入统计 | 否则这类问题继续隐身（本轮就是靠 abort 率反推出来的） |

---

## 五、数据

| 文件 | 说明 |
|---|---|
| `experiments/results/formal-local-20261031-184338.csv` | 756 case，本地 |
| `experiments/results/formal-cross-host-20261031-191357.csv` | 432 case，跨主机 |
| `experiments/logs/formal-both.out` | 两阶段串联日志（含起止时间与 RC） |

原始 CSV 按 CONSTRAINTS 第四条不入库，校验和：

```text
sha256(local) = d18d12837e6caba81ff53a8b6ba6b8b8c2b64a2267532708aa9379e10c223f68
sha256(cross) = 3f1c7c812ab7c425aa59b99231898300303c64be1d9ab66cb31da5128980d1e1
```

复现：

```bash
cd /home/dell/RdmaRangeLock
scripts/run_formal_matrix.sh --mode local
scripts/run_formal_matrix.sh --mode cross-host   # 需要 .66/.67 与 deploy key
scripts/analyze_formal_both.py
scripts/plot_formal_both.py
```
