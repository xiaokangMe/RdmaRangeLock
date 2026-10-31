# BTreeLatch：existing-key update 与 new-key insert 拆分测量（2026-11-01）

对比提交：`50fd8a5`（基线）→ `03eb28e`（引入 `update_existing` 快路径）
机器：`.66`，Xeon Gold 5218 ×2 / 64 线程 / 125 GB，Release（`-O3 -DNDEBUG`）

## 一、结论先给

| 问题 | 回答 |
|---|---|
| existing-key update 多线程是否扩展更好？ | **是**。t1 5.65M → t32 3.48M ops/s，t32/t1 从 **0.18 提升到 0.62**，同线程数下最快提速 **4.05×**（t32） |
| new-key insert 是否仍被 writer/root split 限制？ | **是，而且比以前更差**。t32/t1 仅 **0.10**；且 03eb28e 的 insert 全面慢于 50fd8a5——t1 慢 1.8×，t32 慢 2.4× |
| 32 线程是否出现结构错误 / size 错误 / validate 失败？ | **没有**。72 个 case 全部 `validate_ok=1`、`failed=0`、size 精确（4096 + 16,000,000 = 16,004,096）、树高 2→4 |

一句话：**优化把更新路径从全局写锁里解放出来了，但代价压在了插入路径上**——
`insert()` 现在对每个新 key 先做一次完整下降去找它（必然找不到），再从写路径重新下降一次。

## 二、实验设计

### 两个专用负载

新增 `benchmarks/latch_path_bench.cpp`，一个进程只跑一种模式：

- **update-only**：先把 `[0, keys)` 全部插入（预填充），然后所有操作都是
  `insert(existing_key, value)`——走的正是 `update_existing()` 快路径。
- **insert-only**：预填充 `keys` 个 key 后，所有操作插入**从未存在过**的 key，
  必然触发叶分裂，随规模增长触发根分裂。

两个模式共用一个入口 `BTreeLatch::insert()`，这样 update-only 测的就是真实 API 调用路径，
不是测试专用捷径。

### 关键设计选择

| 选择 | 原因 |
|---|---|
| 线程间 key 区间**不相交** | 目标是测结构瓶颈（`root_guard_` / `writer_guard_`），不是同 key 逻辑冲突。若共享热点 key，测出来的会是缓存行和 latch 争用，无法归因 |
| 插入用连续升序、块间不重叠 | 每个线程在自己区域内持续增长，保证频繁叶分裂与根分裂；同时避免线程间抢同一叶 |
| 初始化不计时 | 预填充在计时区间之外完成，`elapsed_ns` 只覆盖并行阶段 |
| 每个 case 一个新进程、一棵新树 | 不跨 case 复用状态，排除容量增长和缓存残留的影响 |
| 值存储一次性预分配 | 树里存的是裸指针，`vector` 中途扩容会让已插入的指针悬空——这一条在写第一版时踩到过 |

### 矩阵

```text
mode      = update, insert
order     = 128
threads   = 1, 2, 4, 8, 16, 32
runs      = 3            （seed = 8, 9, 10）
update    : prefill 65,536 keys, 每线程 2,000,000 ops
insert    : prefill  4,096 keys, 每线程   500,000 ops
```

两个模式的 op 数不同，是因为两条路径的速度差一个量级：若统一取 500k，update 在 t1
只有 0.09 秒，计时噪声过大；若统一取 2M，insert 在 t32 要跑 4 分钟以上。
**同一模式内两版本用完全相同的参数**，横比仍然有效。

## 三、结果

单位 ops/s，三次重复取中位数。

### update-only

| threads | 50fd8a5 | 03eb28e | 提速 | 03eb28e 相对 t1 |
|---:|---:|---:|---:|---:|
| 1 | 4,819,680 | 5,649,710 | 1.17× | 1.00× |
| 2 | 3,088,520 | 5,110,980 | 1.65× | 0.90× |
| 4 | 1,898,560 | 4,908,820 | 2.59× | 0.87× |
| 8 | 1,246,740 | 3,716,010 | 2.98× | 0.66× |
| 16 | 934,976 | 3,740,300 | 4.00× | 0.66× |
| 32 | 858,989 | 3,476,530 | **4.05×** | 0.62× |

t32/t1：**50fd8a5 = 0.18 → 03eb28e = 0.62**。

单线程也快了 17%——原因是快路径用 shared latch 下降、只在叶上取一次排他锁，
既省掉 `writer_guard_` 的获取，也避免了沿途每个节点都拿排他 latch。

### insert-only

| threads | 50fd8a5 | 03eb28e | 03eb28e/50fd8a5 | 03eb28e 相对 t1 |
|---:|---:|---:|---:|---:|
| 1 | 5,315,060 | 2,897,940 | 0.55× | 1.00× |
| 2 | 3,168,620 | 1,380,160 | 0.44× | 0.48× |
| 4 | 1,903,690 | 1,183,770 | 0.62× | 0.41× |
| 8 | 1,281,970 | 937,766 | 0.73× | 0.32× |
| 16 | 823,598 | 318,666 | 0.39× | 0.11× |
| 32 | 707,952 | 300,708 | 0.42× | 0.10× |

t32/t1：50fd8a5 = 0.13 → 03eb28e = 0.10。

**插入路径整体回退**：同一份代码的写路径没有改动，慢的部分全部来自新的前置探测。

### 完整性（32 线程，中位数）

| build | mode | validate | size | height | committed | failed |
|---|---|---|---|---|---|---|
| 50fd8a5 | update | 1 | 65,536 → 65,536 | 3 → 3 | 64,000,000 | 0 |
| 03eb28e | update | 1 | 65,536 → 65,536 | 3 → 3 | 64,000,000 | 0 |
| 50fd8a5 | insert | 1 | 4,096 → 16,004,096 | 2 → 4 | 16,000,000 | 0 |
| 03eb28e | insert | 1 | 4,096 → 16,004,096 | 2 → 4 | 16,000,000 | 0 |

72 个 case（2 模式 × 6 线程数 × 3 次 × 2 版本）中，`validate_ok=0` 或 `failed>0` 的数量为 **0**。
size 与 "预填充 + 插入总数" 完全吻合，说明 `size_` 计数在并发下没有丢更新；
height 由 2 升到 4，确认根分裂确实发生过。

## 四、为什么 insert 变慢了

`03eb28e` 的 `insert()` 现在是：

```cpp
if (update_existing(key, value)) return RCOK;   // 对每个新 key 必然失败
std::lock_guard<std::mutex> writer(writer_guard_);
...
auto right = insert_recursive(*root_, key, value, separator);  // 第二次下降
```

对新 key 而言，`update_existing()` 是一次**必然失败**的完整下降：

1. 取 `root_guard_` 共享锁；
2. 逐层取 shared latch、逐层释放父节点；
3. 到叶上取排他锁，`lower_bound` 找不到 key，返回 false。

然后才是原来的写路径：`writer_guard_` → 从根开始第二次下降 → 叶分裂。
所以每次插入现在付出 **两次下降 + 一轮 shared latch 往返**，而且第一轮下降的
shared latch 流量与写者的排他 latch 直接争用缓存行，线程越多损耗越大——
这解释了为什么回退幅度随线程数放大（t1 1.8×，t32 2.4×）。

对照 update-only 单线程 5.65M 与 insert-only 单线程 2.90M，后者约为前者的 1.95 倍开销，
与"多一次完整下降"的估计吻合。

## 五、可选的处理方向（未实施）

1. **让探测变便宜**：在叶子之上维护一个 `max_key` 或按页的高位位图，先用常量时间判断
   "这个 key 可能不存在"，直接跳过 `update_existing()`。插入负载下可完全消掉这次浪费的下降。
2. **把判断挪进写路径**：`writer_guard_` 之后的那次下降本来就会在叶子上判断 key 是否存在，
   可以让它对已存在的 key 也走"只改 value、不分裂"的分支——但这样更新又要排 `writer_guard_`，
   把优化还回去了。所以只能二选一，或者按负载选择。
3. **按负载选路**：`BTreeLatch` 增加一个 `expect_insert_heavy` 提示，插入型负载跳过前置探测。
   这是最省事、也最诚实的做法——两种负载的最优路径本来就不同。

## 六、数据与复现

原始 CSV（按 CONSTRAINTS 第四条不入库，位于 `.66`）：

| 文件 | sha256 | 行数 |
|---|---|---|
| `experiments/results/latch-path-03eb28e-20261031-173528.csv` | `e75d19e4761f0460e31bd1dc9d27635d69d7e3b360e100179a17a743ad29a1b0` | 37（1 表头 + 36 case） |
| `experiments/results/latch-path-50fd8a5-20261031-174234.csv` | `25987e1974043ec9d73efee4f42411f2e9979d63cdb04280f04618f6c12ff9df` | 37（1 表头 + 36 case） |

逐 case 日志：`experiments/logs/latch-path-<label>-<stamp>/`；
矩阵输出：`experiments/logs/latch-new.out`、`experiments/logs/latch-old.out`。

```bash
cd /home/dell/RdmaRangeLock

# 当前版本
scripts/run_latch_path_matrix.sh --binary build-bench/rrl_latch_path_bench --label 03eb28e

# 基线版本（独立 worktree，避免污染主工作区）
git worktree add --detach /tmp/rrl-50fd8a5 50fd8a5
cp benchmarks/latch_path_bench.cpp /tmp/rrl-50fd8a5/benchmarks/
cd /tmp/rrl-50fd8a5 && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  && cmake --build build --target rrl_index_latch -j8
g++ -O3 -DNDEBUG -std=c++17 -I include benchmarks/latch_path_bench.cpp \
  build/librrl_index_latch.a -o /tmp/bench-50fd8a5 -pthread
cd /home/dell/RdmaRangeLock && scripts/run_latch_path_matrix.sh \
  --binary /tmp/bench-50fd8a5 --label 50fd8a5

# 汇总
scripts/analyze_latch_path.py --old 50fd8a5 --new 03eb28e
```

基线版本通过独立 worktree 构建：`latch_path_bench.cpp` 只依赖 `BTreeLatch` 的
`init/insert/size/height/validate` 五个方法，两个提交上签名一致，因此同一个基准源文件
可直接链接旧版的 `librrl_index_latch.a`，编译参数与新版保持一致（`-O3 -DNDEBUG`）。

## 七、限制

- **两点未覆盖**：只测了 insert 与 update，没有测 erase（`erase` 仍走 `writer_guard_` 且不重建父节点分隔键，
  并发删除的正确性不在本轮范围内）。
- **key 分布**：update 用的是线程内伪随机（splitmix64），insert 用的是连续升序。
  真实负载的插入通常更接近随机，随机插入会让叶分裂更分散，写路径的争用结构与升序不同。
- **不相交区间是刻意的**：本轮测的是结构瓶颈。若要测同 key 争用，应另设计共享热点负载，
  两者的结论不能混用。
- **未做跨核绑核**：进程可自由迁移，t32 的数据包含 NUMA 效应，未做 pinning。
