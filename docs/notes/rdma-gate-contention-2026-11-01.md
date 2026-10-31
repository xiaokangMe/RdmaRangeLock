# RDMA 锁表：容量已够，但 gate 路径把并发打死了（2026-11-01）

验证对象：`a2c68f8`，`.66` 本机回环（`rrl_rdma_lock_server 19940` + `127.0.0.1` 客户端）
目的：确认 `kRemoteLockSlotsPerShard` 从 64 提到 256 之后，单个事务的持锁数是否还会撑穿分片。

**结论：容量不再是瓶颈（`lock_capacity_aborts` 恒为 0）；但 t≥2 时几乎所有事务都中止，
原因与容量、与热点冲突都无关，是分片 gate 的 all-or-nothing 获取方式造成的。**

本文只做诊断，未修改任何代码。

---

## 一、容量确认：已不再顶穿分片

`rdma-next-key`，ycsb-e，200 事务，单线程（排除并发干扰），key=10,000，zipf=0.99：

| range | 加锁次数 | conflicts | capacity_aborts | gate_aborts | committed |
|---:|---:|---:|---:|---:|---:|
| 8 | 19,100 | 0 | **0** | 0 | 200 / 200 |
| 32 | 64,701 | 0 | **0** | 0 | 200 / 200 |
| 128 | 245,314 | 0 | **0** | 0 | 200 / 200 |

range=128 时单事务峰值约 1,200 把锁。分片映射是 `key / bucket_width % 16`（`bucket_width` 默认 64），
连续 128 个 key 只跨 2~3 个分片，所以单片峰值约 50 把，距离 256 的槽上限还有 5 倍余量。

按条带结构估算：分片 `s` 覆盖的是「每 1024 个 key 取 64 个」的条带，
一个事务要把**单片**塞满 256 槽，需要在一片的条带里同时占住 256 个不同 key ——
换算成连续扫描大约是 range > 16,000。矩阵里最大 range=128，余量两个数量级。

**这一条可以结案：64 → 256 的改动达到了目的。**

---

## 二、并发下的中止全部来自 gate 路径

同一个 workload，range=128，400 事务，10 ops：

| threads | committed | aborted | conflicts | capacity | gate |
|---:|---:|---:|---:|---:|---:|
| 1 | 400 | 0 | 0 | 0 | 0 |
| 2 | 200 | 200 | 200 | 0 | 0 |
| 4 | 100 | 300 | 300 | 0 | 0 |
| 8 | 50 | 350 | 350 | 0 | 0 |
| 16 | 25 | 375 | 375 | 0 | 0 |
| 32 | 13 | 387 | 387 | 0 | 0 |

三条反证，说明这不是锁冲突：

**（1）与热点结构无关。** 四种组合给出完全相同的计数：

| 配置 | committed | aborted | conflicts |
|---|---:|---:|---:|
| range=8, zipf=0.99, t=32 | 13 | 387 | 387 |
| range=128, zipf=0.99, t=32 | 13 | 387 | 387 |
| range=128, zipf=0, t=32 | 11 | 389 | 389 |

如果中止来自范围重叠，range 从 8 到 128（足迹差 16 倍）、zipf 从 0.99 到 0（热点分布完全改变）
不可能得到同一个数字。

**（2）与锁量无关。** 把事务缩到最小：`ops=1, range=1`，每事务只有 2 把锁。

| threads | committed | aborted | 每事务锁数 |
|---:|---:|---:|---:|
| 1 | 400 | 0 | 2 |
| 2 | 200 | 200 | 2 |

两把锁的事务在任何合理的分片容量下都不可能顶穿 256，却依然恰好一半中止。

**（3）v6 路径的中止是货真价实的 gate 失败。**
`rdma-hierarchical`，range=128，bucket=64，t=32：

```text
transactions=400 committed=13 aborted=387
lock_acquires=888 lock_conflicts=0 lock_capacity_aborts=0 lock_gate_aborts=387
```

`lock_conflicts` 为 0，387 次中止全部记在 `lock_gate_aborts` 上。

---

## 三、根因：全分片 gate 是「一次抢齐、抢不到就整体放弃」

### 3.1 `unlock_all` 抢不到门就一把锁都不释放

`src/lock/remote_lock_manager.cpp:346`

```cpp
RC RemoteLockManager::unlock_all(txnid_t txn_id) {
    if (!connected()) return Abort;
    std::vector<std::size_t> ids(kRemoteLockShards);      // 16 片全部
    for (std::size_t i = 0; i < ids.size(); ++i) ids[i] = i;
    std::vector<std::size_t> acquired;
    if (!enter_shards(ids, acquired)) return Abort;       // ← 抢不到就整体放弃
    ...
```

`enter_shards`（`:155`）是 no-wait 的：按升序逐片 `CAS(gate, 0 → owner)`，
任何一片被别人占着就释放已抢到的、返回 false。

而在 `lock()` 里，一个事务只按自己的区间取 1~3 片（`shard_ids`，`:124`）。
于是 t≥2 时几乎必然出现：线程 A 正握着某片做加锁，线程 B 的 `unlock_all` 要 16 片全齐，失败。

失败路径**直接 `return Abort`，一个槽位都不删**。这批锁就永久留在表里：
它们的 `txn_id` 属于已经结束的事务，对后续事务来说就是「别人持有的锁」，
于是下一次 `lock()` 判冲突、`AbortConflict` 加一。

这解释了全部现象：中止数与锁量无关（因为触发点是解锁失败，不是加锁失败）、
与热点无关（残留锁覆盖的是那个线程曾经扫过的区域，与后续是否重叠只是概率问题）、
而且每次运行结果高度可重复（每个进程的连接 token 固定，残留集合确定）。

**这个失败是静默的**：`unlock_all` 的返回码在 `run_next_key_lock` / `run_hierarchical_lock`
里被忽略，`lock_gate_aborts` 只统计 `lock()` 的 gate 失败，所以事件计数上看不见。

### 3.2 无界区间直接映射到全部 16 片

`src/lock/remote_lock_manager.cpp:126`

```cpp
if (!range.has_start || !range.has_end) {
    ids.resize(kRemoteLockShards);       // 无界 → 16 片全部
    for (std::size_t i = 0; i < ids.size(); ++i) ids[i] = i;
    return ids;
}
```

v6 的根意向锁用的正是 `KeyRangeEnt::unbounded()`（`run_hierarchical_lock` 里
`acquire(txn_id, IS/IX, KeyRangeEnt::unbounded(), LockKind::Node)`），
所以**每一次**根锁申请都要一次抢齐 16 个门。32 线程下这几乎必然失败，
直接体现为 `lock_gate_aborts` 占满全部中止。

顺带：`get_lock_count`（`:377`）同样一次性抢 16 片，失败直接返回 0，
会让上层误以为「这个事务没持锁」。

---

## 四、影响

1. **跨主机完整矩阵现在不能跑。** t>1 时 RDMA 路径的有效提交率约 3%（13/400），
   且中止构成与锁协议无关。此时跑出来的「rdma-next-key vs rdma-hierarchical」
   曲线测的是 gate 竞争，不是锁协议。
2. **本地锁表不受影响。** `LockManager` 的分片是按区间取片的，且 `unlock_all`
   逐片处理（见 `src/lock/lock_manager.cpp`），没有 all-or-nothing 问题。
3. **单线程 RDMA 的结果仍然可用。** t=1 时 gate 无竞争，0 中止，
   容量与语义都正常 —— 这一档的数据可以进文章，标注为「单客户端基线」。

---

## 五、修复方向（未实施）

按改动量从小到大：

1. **`unlock_all` / `get_lock_count` 改成逐片处理。**
   进一片、删该片的槽、放该片的门，再下一片。放弃 all-or-nothing，
   即使中途失败也只留下部分未清理，而不是整批失败。这一步不动协议语义，收益最直接。
2. **`lock()` 对无界区间不要抢全部门。** 要么把根锁降级成「逐片加一个 unbounded 锁」
   （语义等价，只是多几次往返），要么给全树元数据单独一个不参与分片竞争的 gate。
3. **把 `unlock_all` 的失败计入统计。** 新增一个 `lock_release_failures` 字段，
   避免这类问题下一次又隐身。
4. **重新基线化。** 修完用本文第二节那张表的参数复测：中止率应当随热点升高而回升，
   而不是像现在这样与锁量、与 range、与 zipf 都无关。

---

## 六、复现

```bash
cd /home/dell/RdmaRangeLock
./build-bench/rrl_rdma_lock_server 19940 &

# 容量确认（单线程、无竞争）
for R in 8 32 128; do
  ./build-bench/rrl_workload_runner --index v2 --cc rdma-next-key \
    --workload ycsb-e --keys 10000 --txns 200 --ops 10 --range $R \
    --threads 1 --seed 8 --zipf 0.99 --rdma-host 127.0.0.1 --rdma-port 19940 --format csv
done

# 并发下的假冲突
for T in 1 2 4 8 16 32; do
  ./build-bench/rrl_workload_runner --index v2 --cc rdma-next-key \
    --workload ycsb-e --keys 10000 --txns 400 --ops 10 --range 128 \
    --threads $T --seed 8 --zipf 0.99 --rdma-host 127.0.0.1 --rdma-port 19940 --format csv
done

# v6 的 gate 中止
./build-bench/rrl_workload_runner --index v2 --cc rdma-hierarchical \
  --workload ycsb-e --keys 10000 --txns 400 --ops 10 --range 128 --bucket 64 \
  --threads 32 --seed 8 --zipf 0.99 --rdma-host 127.0.0.1 --rdma-port 19940 --format csv
```

CSV 列序：`... rows_rechecked, lock_acquires, lock_conflicts, lock_capacity_aborts,
lock_gate_aborts, throughput_committed_txn_s, throughput_attempt_txn_s, checksum`。

本机回环在 `03eb28e` 之后可用（见 `docs/notes/rdma-v5-v6.md` 第 29 节），
所以这些诊断不需要占用 `.67`。

## 七、与既有文档的关系

- `docs/notes/formal-matrix-code-issues-2026-10-31.md` 的 **C-4（槽容量）** 可据此关闭：
  256 槽/片对当前矩阵规模已经足够。
- 同一份文档里「跨主机中止率反映容量而非冲突」的判断需要修正：
  容量确实不再是原因，但中止率同样不是冲突，而是 gate 获取失败与解锁泄漏。
- `docs/notes/rdma-v5-v6.md` 里「gate 是全局 no-wait 元数据锁」的描述仍准确，
  但当时没有意识到它在多线程下会让 `unlock_all` 整体失败。
