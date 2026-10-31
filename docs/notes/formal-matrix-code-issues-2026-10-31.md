# 正式矩阵跑完后暴露的代码问题（2026-10-31）

数据来源：`experiments/results/formal-local-20261031-024829.csv`（756 case，0 失败）
图表：`figures/formal/`，由 `scripts/plot_formal_matrix.py` 生成
报告：`docs/reports/formal-matrix-2026-10-31.html`

本文只列**代码层面**的问题，每条都带实测证据。运行环境与流程类问题见
`formal-matrix-run-findings-2026-10-31.md`；本周更早的静态审阅见 `code-review-2026-10-30.md`。

---

## 一、让数据不可用的问题

### C-1 `LockManager` 一把全局锁 + O(n) 全表扫描，多线程维度失效

- 位置：`include/lock/lock_manager.h:43`（`mutable std::mutex mutex_`）、
  `src/lock/lock_manager.cpp` 中 `lock()` / `unlock()` 对 `std::vector<RangeLock>` 的线性遍历
- 现象：每次加锁都要持同一把互斥量、并线性扫描当前全部持锁记录。事务持锁越多，临界区越长，
  线程数上去是二次方退化。
- 证据（ycsb-e, range=8, t1→t32，committed txn/s）：

  | cc | t1 | t32 | 衰减 |
  |---|---|---|---|
  | record-2pl | 78,289 | 545 | 144× |
  | gap-lock | 54,844 | 437 | 125× |
  | next-key | 58,687 | 505 | 116× |
  | hierarchical | 150,677 | 7,493 | 20× |

  衰减幅度与"每事务持锁数"完全同序（gap-lock > next-key > record-2pl > hierarchical），
  说明测到的是临界区长度，不是并发度。
- 影响：**矩阵的 threads 维度目前没有解释力**。任何"某协议在 t32 更快"的结论都只是在说
  "它更早/更晚撞上同一把互斥锁"。
- 建议方向：锁表按 key 区间分片、每片独立互斥量；冲突判定用有序容器（如 `std::map` 按
  `StartKey` 索引）做区间查询，把 O(n) 降到 O(log n + k)。这一步做完之前，threads 维度
  不应进文章。

### C-2 `BTreeLatch` 的根锁让无锁基线也不扩展

- 位置：`include/index/btree.h`（`mutable std::shared_mutex root_guard_`、
  `mutable std::mutex writer_guard_`）、`src/index/v2_btree_latch.cpp`
- 现象：`none`（完全不取事务锁）从 t1 到 t32 也大幅下降。
- 证据：YCSB-A 243,298 → 42,766（5.7×）；YCSB-E 176,310 → 68,343（2.6×）；
  TPC-C payment 550,999 → 176,159（3.1×）。
- 影响：这是**与事务锁无关的第二瓶颈**。当前所有曲线都是"索引层根锁 + 锁表全局锁"两个
  瓶颈叠加的形状，无法把差异归因到锁协议本身。CONSTRAINTS 第一条要求"性能差异可归因"，
  这一条现在不满足。
- 建议方向：先单独测 `rrl_v2_btree_latch_test` 的纯索引扩展曲线（不接事务层），
  确认根锁是不是主因；若是，考虑 root split 只在树高变化时持写锁。

### C-3 YCSB-E 的插入永远落在扫描区间之外，幻读防护未被测量

- 位置：`src/txn/workload.cpp` 的 `ycsb()`
- 现象：范围读区间为 `[k, min(key_count, k + range_length))`，插入 key 为
  `key_count + n * operations_per_transaction + i`，恒 ≥ `key_count`，两者系统性不相交。
- 证据：YCSB-E 全部线程数、全部协议的 `aborted` 均为 **0**（含 record-2pl）。
- 影响：v3（只锁记录）在这一轮不会产生幻读，v4/v5 防住的幻读本来就不存在。
  三者之间的差别只剩 `lock_acquires`。**项目核心命题（范围锁防幻读）在现有数据里没有立足点。**
- 建议方向：插入 key 改为落在最近一次扫描区间内部（例如从 `[k, k+range)` 中取一个未使用的 key），
  或引入独立的"幻读构造"工作负载（写者插入读者刚扫过的区间）。

### C-4 远程锁表每片 64 槽，容量上限被当成冲突统计

- 位置：`include/lock/remote_lock_manager.h`（`kRemoteLockShards = 16`、
  `kRemoteLockSlotsPerShard = 64`）、`src/lock/remote_lock_manager.cpp` 的 `lock()`
  中 `if (table.size() == kRemoteLockSlotsPerShard) return Abort;`
- 现象：分片按 `key / bucket_width % 16` 映射，一次范围扫描锁的是连续 key，天然挤在相邻分片。
- 证据（跨主机，单线程、单客户端，`lock_conflicts` 恒为 0）：

  | range | 每事务持锁 | aborted |
  |---|---|---|
  | 1 | 20 | 0 |
  | 4 | 48 | 0 |
  | 16 | 159 | 7 |
  | 32 | 258 | 51 |

- 影响：跨主机 432 个 case 一个都跑不了；即便跑，中止率反映的是槽位不够，不是锁冲突。
  默认矩阵 range 取 8/32/128，后两档必然被容量吃掉。
- 建议方向：槽数按"单事务最大持锁数"估算后放大（256~512），并把 capacity 失败与
  conflict 失败分成两个返回码，别都塞进 `Abort`。

---

## 二、会让结论被误读的问题

### C-5 bucket 宽度硬编码 64，而它的最优值随负载反转

- 位置：`WorkloadConfig::hierarchy_bucket_width` 的默认值 64，以及
  `run_hierarchical_lock` 中 `bucket()` 的固定切分
- 证据（t1→t32 的 committed txn/s）：

  | 负载 | bucket=16 | bucket=64 | bucket=256 |
  |---|---|---|---|
  | YCSB-E（扫描为主） | 最低 | 中 | 最高 |
  | YCSB-A（写冲突为主） | 最高 | 中 | 最低 |

- 影响：矩阵取 64 是折中，**两个场景都不占优**。若文章只跑 64 就下"层级锁更好/更差"的结论，
  换一个 bucket 就能把结论推翻。
- 建议方向：bucket 宽度做成配置项，报告里同时给 16/64/256 三条曲线；并在正文写清
  "粗桶降低锁数量、细桶降低冲突面"这个反向权衡。

### C-6 TPC-C payment 不遵守 operations 参数，与 new-order 不可横比

- 位置：`src/txn/workload.cpp` 的 `payment()`
- 现象：`payment()` 固定生成 3 个 `Update`，`operations_per_transaction` 对它完全无效；
  `new_order()` 才有 `tpcc_items_per_order` 循环。
- 证据：t1 吞吐 payment 550,999 vs new-order 224,097，差 2.5 倍。
- 影响：两者放在同一张图里会被读成"payment 更轻"，实际是事务成本不同构。
- 建议方向：要么让 payment 也按 ops 展开，要么在报告与图注里明确标注两者事务结构不同。

### C-7 no-wait 的 `Abort` 把"冲突"与"容量不足"混成一个返回值

- 位置：`src/lock/lock_manager.cpp`（冲突即 `return Abort`）、`include/common/types.h`
  里定义了 `WAIT` / `WAIT_REM` 但从未返回
- 证据：YCSB-A t32，record-2pl attempted 35,388 / committed 3,740（89.5% 中止）；
  hierarchical attempted 40,498 / committed 421（99.0% 中止）。
- 影响：`aborted` 无法区分"锁冲突"、"槽位不足"、"gate 抢占失败"三种原因，
  这三者在诊断上完全不同。同时 attempted 吞吐自身也随线程下降（129k → 35k），
  说明瓶颈是事务变慢、持锁变长，不是重试策略。
- 建议方向：至少拆成 `AbortConflict` / `AbortCapacity` / `AbortGate` 三个返回码，
  分别计数进 CSV。

### C-8 矩阵脚本：单点失败终止整轮，配置文件从未被读取

- 位置：`scripts/run_formal_matrix.sh`（`run_case` 的 `return 1` + 顶部 `set -euo pipefail`）、
  `experiments/configs/*.json`
- 现象：① 任一 case 非零退出即 `return 1`，配合 `set -e` 让整轮退出；本轮 r=128 t8 单例
  已跑到 118 秒，t32 极可能撞上 900 秒上限并连带丢掉后面约 250 个 case。
  ② `experiments/configs/` 下的 8 个 json 没有任何代码读取，`workload_runner` 只解析命令行参数。
- 影响：一次超时可能丢掉整轮数据；同时 CONSTRAINTS 第四条"配置能一条命令复现"不成立——
  配置文件与命令行是两套彼此独立的东西。
- 建议方向：`run_case` 失败只记录并 `continue`，整轮结束后汇总失败清单；让 runner 支持
  `--config file.json`，或让矩阵脚本从 json 生成命令行。

---

## 三、工程层面

### C-9 绘图脚本依赖仓库外的 Python 环境

- 位置：`scripts/plot_formal_matrix.py`
- 现象：需要 matplotlib + numpy。`.66` 上原本没有，本轮通过 `pip3 install --user` 现场安装；
  仓库里没有 `requirements.txt` 或任何依赖声明。
- 影响：违反 CONSTRAINTS 第六条"构建必须能从干净 clone 一次成功"。
- 建议方向：加 `scripts/requirements.txt`，或在脚本开头做 import 检查并给出安装提示。

### C-10 `.gitignore` 未覆盖 `build-*`

- 位置：`.gitignore:2` 只有 `build/`
- 现象：`build-formal/` 在 `git status` 里以未跟踪状态出现。
- 建议方向：改成 `build*/`。

---

## 四、可以关闭的旧条目

对照 `code-review-2026-10-30.md`：

| 旧条目 | 状态 |
|---|---|
| P0-17 `build-rdma` 缺失、脚本必失败 | 已关闭，改为 `build-formal` + 脚本内构建 |
| P1-13 吞吐用 transactions 而非 committed | 已关闭，CSV 现有 committed / attempt 两列 |
| P2-16 v5 `rows_scanned` 恒为 `rows_found` 两倍 | 已关闭，重扫独立记为 `rows_rechecked`；实测 `rows_scanned == rows_found` |
| P0-8 RDMA 单 QP 串行 | 部分关闭，已实现 per-thread QP 池（`set_connection_count`） |
| 其余（gate 无租约、CQ 无超时、`get_lock_count` 撕裂读等） | 仍未处理 |

---

## 五、建议顺序

1. **C-3**（YCSB-E 插入位置）——不做这一步，v3/v4/v5 的对比在文章里没有立足点，成本也最低。
2. **C-1 + C-2**（两个串行点）——不做这一步，threads 维度整体作废；先测纯索引扩展曲线定位主因。
3. **C-5**（bucket 可配 + 三档曲线）——直接决定 v6 那节怎么写。
4. **C-4**（槽数放大 + 失败分型）——跨主机半张矩阵的前置条件。
5. **C-6 / C-7 / C-8**（口径与脚本）——影响可读性，不影响数据本身。
6. **C-9 / C-10**——顺手。
