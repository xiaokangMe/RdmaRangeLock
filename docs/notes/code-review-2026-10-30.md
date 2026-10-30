# 代码审阅问题清单（2026-10-30）

审阅对象：`RdmaRangeLock` @ `dbc948d`（接入 RDMA 远程锁表并运行 v5 v6）
审阅方式：只读静态阅读 + 与 `experiments/results/` 既有数据比对
**本文只记录问题，不含任何修复。** 修复方案另开 `docs/notes/` 文件并单独提交。

---

## 一、正确性风险

### P0-1 gate 无租约，客户端异常退出即永久封锁锁表

- 位置：`src/lock/remote_lock_manager.cpp:34`（`enter_gate`）、`:87`、`:129`、`:149`
- 现象：`gate` 靠 `RDMA CAS(0 → owner_token)` 抢占，释放靠 `leave_gate()` 无条件写 0。客户端在持门期间崩溃（段错误、被 kill、网络断），gate 永远停在非 0 值。
- 影响：此后**所有** `lock` 调用在 `enter_gate()` 处失败并返回 `Abort`，表现为"实验能跑完、但 100% 中止"。这类失败不会报错，只会让统计数字静默变坏，比崩溃更危险。
- 现状：`docs/notes/rdma-v5-v6.md` 已承认需要 lease / 故障恢复，但代码里没有任何超时或 owner 校验。
- 建议方向：服务端或 gate 值里带时间戳做 lease；或客户端连断时由服务端清零（`wait_for_disconnect` 已经有钩子）。

### P0-2 `post_and_poll` 无超时，远端失联直接挂死

- 位置：`src/rdma/rdma_transport.cpp:215-223`
- 现象：`while (ibv_poll_cq(...) == 0) std::this_thread::yield();` 是纯自旋，没有任何超时或重试上限。
- 影响：对端进程被杀 / 网卡 down 时，调用线程永久空转吃满一个核，且**此时进程往往正持有 gate**，把 P0-1 的封锁从"一次"放大成"永久 + 烧 CPU"。
- 建议方向：CQ 轮询加 deadline，超时返回 `ERROR` 并在上层触发重连。

### P1-3 `get_lock_count` 不抢 gate，读到撕裂快照

- 位置：`src/lock/remote_lock_manager.cpp:164-175`
- 现象：其它三个方法都先 `enter_gate()`，只有 `get_lock_count` 直接 `load_slots`，与别的客户端的写入并发。
- 影响：返回值可能既不是加锁前也不是加锁后的状态。作为统计指标（锁数量是 v4/v5/v6 的核心对比维度之一）不可信。
- 备注：该问题目前被调用方的写法部分掩盖（v5/v6 里只在 `unlock_all` 前后调用），但一旦拿它做锁数量统计就会出错。
- 建议方向：与其它方法一致先抢门；或明确降级为"仅用于调试、不进论文数据"。

### P1-4 父意向锁判定不对称

- 位置：`include/common/range.h:102-109`
- 现象：自身判定"无条件区间"用 `!has_start && !has_end`，对端判定却只看 `other.range.has_start`：

  ```
  if (this_parent_intention && other.range.has_start) return false;
  if (other_parent_intention && range.has_start)      return false;
  ```

- 影响：只有终点、没有起点的区间（`KeyRangeEnt::to_right(...)`）走到这里会被当成"有起点"处理，两边的判定口径不一致。当前 v6 路径只用 `unbounded()` 做根、`closed(base,end)` 做桶，暂时踩不到；但接口是对外暴露的公共头文件，语义不自洽。
- 建议方向：抽一个 `is_unbounded()`（`!has_start && !has_end`）两边共用，不要各写一套。

### P1-5 `unlock` 匹配不比较 `kind`

- 位置：`src/lock/lock_manager.cpp:36-45`
- 现象：`lock` 的幂等/升级分支比较了 `held.kind == kind`（`:24`），`unlock` 的 `remove_if` 却只比区间，不比 kind。
- 影响：同一事务对同一区间先后持有 `Record` 与 `Gap` 两把锁时，`unlock` 会把两把一起删掉，破坏"只解自己那一把"的语义。v4 的 gap-lock 路径对同一区间既加 `Gap` 又加 `Record`，是潜在触发点。
- 建议方向：`unlock` 增加 kind 参数或在 `remove_if` 里带上 kind 比对；接口变更按 CONSTRAINTS 第二条单独提交。

---

## 二、性能与读放大

### P0-6 每次加锁整表 64KB 单边读

- 位置：`src/lock/remote_lock_manager.cpp:89`、`:131`、`:151`（三处 `load_slots`）
- 现象：`load_slots` 固定读 `kRemoteLockSlots(1024) × 64B = 64 KiB`，无论表里实际有几把锁、无论要判的是不是点查。
- 影响：`experiments/results/rdma_v5_v6_smoke.txt` 里 v5 一次 smoke 有 12889 次 acquire，仅此一项就是 **≈ 805 MiB** 的单边流量；再加每事务一次 `unlock_all` 的全表读，实际翻倍。
- 数据佐证：同一 workload、同一 index 下，`rdma-next-key` 435.8 txn/s，本地 `next-key` 6536.7 txn/s，掉一个数量级，与"每次锁操作搬 64KB"的模型吻合。
- 建议方向：槽位按 key 分桶，只读相关桶；或维护远端 slot 计数、按有效区间读。属于结构性改造，应先写方案。

### P0-7 `unlock_all` 每事务无条件调用

- 位置：`src/txn/workload.cpp`（`run_next_key_lock` 与 `run_hierarchical_lock` 末尾）
- 现象：v3/v4 还做了 `if (locks.get_lock_count(txn_id) > 0)` 的前置判断，v5/v6 直接 `locks.unlock_all(txn.txn_id);`。
- 影响：本地版只是一次 vector 扫描，代价可忽略；**RDMA 版则是每事务一次 64KB 读 + CAS + 可能若干次 64B 写**，对已 abort、从未持锁的事务完全是浪费。
- 建议方向：要么恢复前置判断（但 `get_lock_count` 本身也是全表读，等于没省），要么在客户端维护本地影子计数。

### P0-8 进程级 `io_mutex_` 把所有线程串成单线程

- 位置：`include/lock/remote_lock_manager.h:64`，使用点 `src/lock/remote_lock_manager.cpp:84`、`:128`、`:148`、`:165`
- 现象：`RemoteLockManager` 只有一条 `RdmaConnection`（单 QP），每个方法进来先拿 `io_mutex_`。
- 影响：`--threads 4` 在 RDMA 路径上实际退化成 1 线程串行。这意味着 **RDMA 版的吞吐数字不能与本地多线程版本直接横比**，两者的并发模型根本不同；现有 `rdma_v5_v6_smoke.txt` 与 `v5_next_key_smoke.csv` 并排放是会误导结论的。
- 建议方向：要么明确标注"RDMA 基线为单 QP 串行，仅验证语义"，要么做 per-thread QP / 多连接池。这一条不修，v5/v6 的 RDMA 性能结论不能进文章。

### P2-9 `read` 双拷贝

- 位置：`src/rdma/rdma_transport.cpp:271-283`
- 现象：数据先进 1 MiB `scratch`，再 `memcpy` 到调用方缓冲。
- 影响：64KB 表读多一次内存拷贝，占比不大但白送。
- 建议方向：`read` 接受调用方缓冲并直接注册，或让调用方传入已注册 MR。

### P2-10 `region_image_` 是死成员

- 位置：`include/lock/remote_lock_manager.h:66`，唯一使用 `src/lock/remote_lock_manager.cpp:25`
- 现象：`connect()` 里 `region_image_.resize(sizeof(RemoteLockRegion))`（约 64KB），之后从未读写。
- 影响：白占 64KB 且误导读者以为有本地影子副本。
- 建议方向：删除，或真的用起来做影子表。

### P2-11 `devices[0]` 选卡

- 位置：`src/rdma/rdma_transport.cpp:129-133`
- 现象：`ibv_get_device_list` 后直接 `ibv_open_device(devices[0])`，没有按名字选 `mlx5_0`，也没有检查端口状态。
- 影响：多卡机器（或有非 RoCE 设备排在前面）会选错设备，症状是握手成功但数据面超时。`.66` 上目前只有一块卡所以没暴露。
- 建议方向：按设备名或 GID index 参数化；至少校验 `PORT_ACTIVE`。

---

## 三、实验与数据规范

### P1-12 部分 `results/*.csv` 内容不是 CSV

- 位置：`experiments/results/v5_next_key_smoke.csv`、`v6_hierarchical_smoke.csv`、`rdma_v5_v6_smoke.txt`
- 现象：这三个文件里是 `workload=ycsb-e,index=v2,cc=...` 的单行 key=value 文本，没有表头、没有列结构；而 `v3_record_2pl_smoke.csv`、`v4_gap_lock_smoke.csv` 是规范 CSV（带表头）。
- 影响：直接违反 `CONSTRAINTS.md` 第四条"数据要能一条命令复现、能出图"。任何按 CSV 解析的脚本读到这三个文件都会失败或得到错误列。
- 建议方向：统一为带表头 CSV；`.txt` 保留为运行日志而非结果。

### P1-13 `throughput` 用 `transactions` 而非 `committed`

- 位置：`src/txn/workload.cpp`（`RunStats::throughput`）
- 现象：`return elapsed_ns ? double(transactions) * 1e9 / elapsed_ns : 0;`
- 影响：`v3_record_2pl_smoke.csv` 里 1000 事务只 committed 359，但吞吐按 1000 算。v3/v4（高 abort）与 v5/v6（低 abort）横比时，吞吐会被系统性高估，方向恰好偏向"冲突多的版本看起来更快"。
- 建议方向：报告里同时给 `committed/s`，或明确约定吞吐口径为 committed。这属于统计口径定义，不改代码也能在文中说明，但目前 `notes/` 里没有写。

### P1-14 no-wait `Abort` 与 2PL 的阻塞语义不等价，但被当成同一件事比较

- 位置：`src/lock/lock_manager.cpp:20-26`（冲突即返回 `Abort`，无 `WAIT`）
- 现象：`RC` 里定义了 `WAIT` / `WAIT_REM`，但锁管理器从不返回，冲突一律 `Abort`。
- 影响：v3/v4 的 641/636 次 abort 中，很大一部分在真实 2PL 里是"短暂阻塞后成功"，不是中止。用它论证"gap lock 阻塞率高"会把阻塞误记成中止，结论方向可能反了。
- 建议方向：至少在各版本说明里写清"本实现为 no-wait，abort 率不等于阻塞率"；若要论证阻塞，需要补等待队列。

### P1-15 v6 smoke 的 6 次 abort 没有任何解释

- 位置：`experiments/results/v6_hierarchical_smoke.csv`（aborted=6）对比 `v5_next_key_smoke.csv`（aborted=0）
- 现象：同一 workload（ycsb-e，200 txn，4 线程，1000 key），v6 用 bucket=64 的桶级 X 锁，v5 用行级 next-key，结果 v6 反而多 6 次冲突。
- 影响：这正是 v6 存在的理由所在（粒度换开销），但"粒度变粗导致更多冲突、换来 lock_acquires 从 12889 降到 1694"这句话在 `notes/` 和 `results/` 里都没有写。数字摆着没人解释，读者只会觉得 v6 更差。
- 建议方向：在 `docs/notes/v6-hierarchical-lock.md` 补一段"桶宽 vs 冲突率 vs 加锁次数"的权衡说明，并给出 bucket=16/64/256 的对照数据。

### P2-16 v5 的 `rows_scanned` 恒为 `rows_found` 的两倍

- 位置：`src/txn/workload.cpp`（`run_next_key_lock` 的 recheck 分支）
- 现象：加锁后再 `scan_range` 一次做校验，`a.scanned += verify.size()`，于是 `rows_scanned = 2 × rows_found`（12089 → 24178）。
- 影响：与 v6（扫描一次，12089 → 12089）横比时，会得出"v5 扫描量是 v6 两倍"的错误结论——实际那是校验开销，不是扫描算法差异。
- 建议方向：拆成 `rows_scanned` 与 `rows_rechecked` 两个计数器。

---

## 四、构建与可复现性

### P0-17 `build-rdma/` 已不存在，RDMA 一键脚本必然失败

- 位置：`scripts/run_rdma_smoke.sh`（写死 `"$ROOT/build-rdma/rrl_rdma_lock_server"`）
- 现状：机器上只有 `build/`（9 个本地二进制：7 个 test + `rrl_workload_runner` + 1），`build-rdma/` 目录不存在，仓库里也没有生成它的脚本或 CMake preset。
- 影响：README 第六节给的三条命令、`scripts/run_rdma_smoke.sh` 全部立刻失败；`experiments/results/rdma_v5_v6_smoke.txt` 目前无法复现。这直接违反 CONSTRAINTS 第七条 DoD 第 3 项"能一条命令复现"。
- 建议方向：要么把 `build-rdma` 的配置方式写进脚本（`cmake -S . -B build-rdma && cmake --build build-rdma`），要么统一到单一构建目录 + CMake option 控制是否编 RDMA。

### P2-18 服务端一次只服务一个客户端

- 位置：`src/rdma/rdma_transport.cpp:253`（`accept` 后立刻 `close(listen_fd)`）、`benchmarks/rdma_lock_server.cpp`
- 现象：`accept_server` 接受一条连接就关掉监听 socket，主循环再重新监听；`RemoteLockRegion` 是 `main` 的栈变量，所有连接共用。
- 影响：串行建连，无法测多客户端争用；`experiments/` 里也没有多机实验。
- 备注意见：`docs/notes/rdma-v5-v6.md` 的"下一步"已提到分片 gate，但没提多客户端。

---

## 五、测试覆盖

### P1-19 v5/v6 只有锁表单测，没有并发幻读用例

- 位置：`test/v5_next_key_test.cpp`（28 行）、`test/v6_hierarchical_lock_test.cpp`（23 行）
- 现象：两者都只顺序调用 `LockManager::lock` 断言返回值，没起线程、没接索引、没构造幻读场景。
- 对照：v3/v4 有 `test/v3_phantom_test.cpp`、`test/v4_gap_lock_test.cpp` 做真正的事务级验证。
- 影响：v5/v6 的"消除幻读"这一核心论断，目前只有锁表层面的间接证据，没有端到端用例。DoD 第 1 条（对应用例通过）在这两个版本上是缺的。
- 建议方向：复用 v4 的 phantom 用例骨架，把 `LockManager` 换成 `IRangeLockManager&`（接口已经就绪，只需模板化或传引用）。

### P1-20 里程碑与实现状态不一致

- 位置：`README.md` 第四节
- 现象：M3（v2 latch coupling 64 核压测 1 小时、无死锁）未勾选，M6（综述初稿）未勾选；其余四项已勾。
- 影响：未勾的两项恰好是"并发正确性"和"文章本体"，也就是这个项目两个最终交付物。当前进度实际停在"六个版本都能跑通 smoke"。
- 备注：不是代码缺陷，但 README 顶部写"核心主张：综述不能只靠讲故事"，而故事还没开始写。

---

## 六、待验证的猜想（尚不确定，先记下）

1. **`KeyRangeEnt(start, 0)` 的 0 陷阱**：两参构造把 `EndKey == 0` 解释为"点"，于是 `KeyRangeEnt(k, 0)` 等于 `point(k)`。`KeyXidCache` 的 deneva 兼容路径 `deneva_range(start, end)` 直接用了这个构造，若上游真有 `end=0` 表示"到 0 为止"的调用，会被静默解释成点锁。需要翻 deneva 原代码确认约定。
2. **v5 recheck 只比 `size()` 和 key 序列**：若期间发生"删除 key X + 插入 key X"，size 与序列都不变，校验通过，但那是两次不同的写。key 唯一时影响有限，`run_workload` 的 `Update` 也不改 key，所以目前碰不到；但作为"校验能挡住幻读"的论证不严密。
3. **`leave_gate` 不校验 owner**：任何客户端都能把 gate 清零。目前单客户端场景无影响，多客户端时若 A 的 gate 被 B 误清，A 会以为自己仍持门。需要结合 P0-1 的 lease 一起设计。
4. **`wire_txn` 的 48 位命名空间**：`namespace_token`（48 位）左移 16 后与 `txn_id` 异或，若两个客户端 token 恰好高位相同、低位互补，事务 ID 会在远端碰撞。概率极低，但把 `owner` 字段与 `txn_id` 分开校验会更稳。

---

## 七、已完成处理

- 2026-10-30：清理了家目录下 12 个名字为字面 `\home\dell\RdmaRangeLock\...` 的空目录（Windows 路径风格误建的空壳，递归文件数 0）。真仓库 `~/RdmaRangeLock` 未受影响，`git status` 干净。此事与代码无关，仅记录以免重复排查。

---

## 八、优先级建议（不含实施）

| 顺序 | 条目 | 理由 |
|---|---|---|
| 1 | P0-17 补构建方式 | 不修则所有 RDMA 数据不可复现，DoD 直接不达标 |
| 2 | P0-1 / P0-2 gate lease + 超时 | 崩溃一次即污染整轮实验，且表现是静默的 |
| 3 | P1-12 / P1-13 / P1-16 数据口径 | 改文档 + 改计数器，成本低，直接决定现有图能不能用 |
| 4 | P0-8 多线程串行标注 | 不改代码也要先标注，否则 v5/v6 横比结论站不住 |
| 5 | P0-6 / P0-7 读放大 | 结构性改造，需先写方案，属于下一阶段 |
| 6 | P1-19 v5/v6 并发用例 | 补上 DoD 第 1 条 |
