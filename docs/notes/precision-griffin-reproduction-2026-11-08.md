# Precision Locks / Griffin：论文复现与验收（2026-11-08）

## 复现范围

本次精读并实现以下两项，不将原有 exact-range 改名充当 Precision Locks：

| 入口 | 机制 | 数据路径 |
|---|---|---|
| index=v2, cc=precision-lock | Jordan 1981 Lp/Lu 完整单键区间子集，含写前隐含读 | 统一本地 B+ 树 |
| index=v2, cc=griffin-pl | Griffin membership PL + record strict 2PL | 统一本地 B+ 树；锁机制消融 |
| index=griffin, cc=griffin-pl | 上述锁 + hash 点访问、key-only B+ 树扫描、异步同步线程 | 本地 Griffin 混合索引 |
| index=rdma, cc=rdma-precision-lock | 与本地相同的 Jordan Lp/Lu | 现有共享远端 B+ 树、锁槽、事务状态 |
| index=rdma, cc=rdma-griffin-pl | 与本地相同的 Griffin membership/value 保护 | 现有共享远端 B+ 树；**不是远端混合 Griffin** |

统一接口、事务私有写集、no-wait / wait-die / wound-wait、重试与延迟统计沿用原系统。新增配置固定 32 OS 线程，execution_mode=threads, coroutines=1。本次不启动正式性能矩阵，不产生新旧性能排序结论。

**Griffin-PPL 不在本次复现之列**：用户目录中 CANDARW 2024 对应文件是题录；十页 Griffin PDF 是 arXiv:2407.13294，不能从 PPL 摘要推测去中心化链表实现。

## 一手依据与源码检索

1. Jordan, Banerjee, Batman. *Precision Locks*, SIGMOD 1981, pp.143–147, DOI 10.1145/582318.582340。用户提供五页全文 PRECISION LOCKS.pdf。§4.1 从印刷页144开始，核心双向检查、隐含读、§4.2–4.3见印刷页145。原文谓词可任意，本项目谓词语言限定 uint64 单键区间，不宣称任意元组谓词。
2. Nakazono, Kawashima, Bessho, Nakamori. *Griffin: Fast Transactional Database Index with Hash and B+-Tree*, arXiv:2407.13294 (2024)。§II-C2 (PDF p.3) 说明 Lp/Lu，§III-B/E/F (pp.5–6) 说明防幻读和防 stale read，算法1/2 (p.6) 说明同步/回收与操作顺序，§IV (p.7) 说明 hash / BwTree / lock-free lists。
3. 已查公开论文网页及 GitHub API，没有找到这两项可直接移植的完整公开实现；Griffin 引用的 OpenBwTree、LineairDB 不是 Griffin 本身。此次依原文独立实现，复用项目 BTreeLatch 和分片锁表，未引入第三方依赖。论文及源码检索结果不意味着源码永久不存在。

## 逐项对应与适配边界

| 原文机制 | 当前代码 | 统一架构适配 |
|---|---|---|
| Jordan §4.1：Lp 读谓词，Lu 实际更新元组；只检查 Lp × Lu | include/common/range.h, src/txn/precision_plan.h | 区间包含一个实际更新 key；Lu 不允许写范围 |
| Jordan §4.1：修改/删除隐含读，唯一键插入隐含不存在检查 | precision_point() | 每个写操作先登记点 Lp 再登记点 Lu，严格禁止遗漏 |
| Jordan §4.2：P(t) 判定，不求谓词交集可满足性 | RangeLock::conflicts() | 单键区间 contains(key)；不是一般表达式求值器 |
| Jordan §4.3：两阶段持锁 | src/txn/policy_workload.cpp | 共用 prepare_commit / 私有写集发布 / 统一释放；无提前释放 |
| Griffin 算法2：Insert/Delete × Scan | GriffinPredicate/GriffinUpdate | 原文发布后检查改为分片内原子检查并发布，排除并发双方漏检 |
| Griffin §V-A：索引实验未覆盖记录内容隔离 | PolicyProtocol::Griffin | 补 record S/X，允许统一多操作事务；不声称是原论文单操作实验 |
| Griffin §IV：hash 点访问，树只存 key，扫描值回 hash 读取 | src/index/griffin.cpp | 标准库分片 hash + 现有 BTreeLatch；不是原文 lock-free hash / OpenBwTree |
| Griffin 算法1：同步后再删除 committed Lu | pending, queue, synchronize() | txn 层 active Lu 正常持有；提交阶段在 index 建立无 txn 身份的 pending 元数据，同步后才清除 |
| Griffin 算法1：abort 回滚未提交 hash 变动 | 共用事务 pending 私有写集 | 改为提交许可前不写 hash，abort 丢弃私有写集，无需物理回滚 |
| Griffin 算法1：专用 sync manager | 专用线程 + FIFO 批次队列 | 条件变量按需唤醒，不采用定时轮询；不实现 PPL 的去中心化替代 |

索引中的 pending 只表示已经提交、尚未复制到有序树的 key，不保存 txn ID、等待策略或事务锁。事务级 Lp/Lu 留在 src/txn 和 src/lock，遵守 index/lock 层边界。

本地混合索引当前使用一个短期共享结构 fence 协调 hash 批次、pending 与树同步；扫描在读取期间持共享 fence，提交/同步持排他 fence。它是结构一致性适配，并非事务级锁；仍是一个性能串行点。不能据此宣称已复现原文无锁实现的扩展性或加速比。

## 正确性不变式

### Precision Locks

- 任何读取（包括空结果）先发布 Lp；实际更新先发布 Lu。
- Lp 对 Lu、Lu 对 Lp 双方向冲突检查。Lp/Lp 与 Lu/Lu 本身均不检查冲突。
- Lu/Lu 可并存不等于允许丢失更新：写操作隐含的点 Lp 与对方 Lu 冲突，保证同键修改与唯一插入互斥。
- Lp/Lu 的判定和发布共用既有分片临界区；一个冲突 Lp/Lu 对不可能并发双成功。
- 原有谓词、记录、gap、next-key 等 kind 的兼容规则不变；追加枚举值，不改旧线格式值或公共接口签名。

### Griffin

- active membership Lu 阻挡相交扫描；扫描 Lp 阻挡相交成员变动。
- 值更新只取 Record X；已存在 key 的 value-only Update 不进 Lu。缺失键 Update 是 upsert，必须取 Lu。
- 扫描先取 Lp，再从有序树取 key，一次遍历后取对应 Record S，再回 hash/统一索引读取当前值。整个事务严格持锁，不依赖第二次 B+ 树扫描防幻读。
- 私有 insert/delete 在提交前不影响 hash；读己之写/删由共用写集覆盖。
- 提交 hash 批次与发布 pending 在同一排他结构 fence 内完成；扫描不能看到二者中间态。
- 提交返回时可能树尚未同步，但相关 pending 仍在。扫描覆盖 pending key 时抛 IndexSyncPending 并中止该 attempt，不能返回缺失或过期 key。
- FIFO 应用完整批次，再撤销对应 pending 计数；同 key 跨批删除/重插不会乱序或提前解保护。
- 队列满在任何 hash 修改之前返回 AbortCapacity；提交中途或同步故障将索引置为 failed，拒绝后续读写，事务层保留不确定写的锁并停止。
- 失败状态在释放 shard/fence 前发布，读取在取得 shard latch 后复查；条件变量失败通知与等待谓词使用同一 fence，避免丢失唤醒。

## 和 exact-range 的关系及比较口径

exact-range/predicate 以 S/X 区间交叠判断冲突，当前写操作是点 X；precision-lock 分离读谓词和实际更新，只判断 predicate.contains(update_key)，不提供写范围。对于当前单键区间、点写负载，两者允许的事务交错可能相同，不能只因新名字就声称更高并发。开销差异需要同数据路径测量。

Griffin 的 PL 只保护成员集合，Record 2PL 另保值；本地混合版还改变了点查索引结构。纯锁机制比较固定 index=v2 或固定 index=rdma；混合索引性能另列一组，避免把 hash 加速误归因给锁算法。

## 配置与复现命令

三份小型正确性配置：
- experiments/configs/precision_lock_correctness.json
- experiments/configs/griffin_pl_correctness.json
- experiments/configs/griffin_hybrid_correctness.json

共同默认：YCSB-E、128 seed keys、96 logical txns、4 ops/txn、range length 8、Zipf 0.4、32 OS threads、wait-die；三策略覆盖由测试脚本完成。数据量较小，用于验收，**不可当正式性能配置**。

~~~bash
cmake -S . -B build-shared -DCMAKE_BUILD_TYPE=Release
cmake --build build-shared -j8
ctest --test-dir build-shared --output-on-failure
build-shared/rrl_workload_runner --config experiments/configs/precision_lock_correctness.json --format csv
build-shared/rrl_workload_runner --config experiments/configs/griffin_pl_correctness.json --format csv
build-shared/rrl_workload_runner --config experiments/configs/griffin_hybrid_correctness.json --format csv
python3 scripts/check_precision_runtime.py --build build-shared
python3 scripts/check_precision_runtime.py --build build-shared --cross-host
build-shared/rrl_griffin_soak_test 3600
~~~

griffin_queue_capacity / --griffin-queue-capacity：默认8192，允许1..1048576，单位是待处理提交批次，包含同步线程正在处理的一个批次，不是 key 数或事务锁槽数。队列满保守拒绝整个新批次，即使该批最终仅改值；这是当前容量行为，不是论文额外保证。

CSV 末尾追加 index_sync_pending_aborts，旧列顺序保持。该项是树同步滞后导致的正常 attempt 中止，计入总中止与重试，**不计入 lock_conflicts**；不能把它解释成网络错误或单纯锁冲突。原有 committed-only 与含重试延迟、p50/p99/p999、阻塞/重试统计继续适用。

## 验收记录

- 已执行源码对应的确定性契约、多操作串行模型、并发发布 race、异步 pending/FIFO/容量/故障测试。
- 本地32线程精确累计：16对键各由两个线程竞争，每线程提交8次；三协议/索引组合 × 三等待策略，共9组，每组各对两键均精确为16，所有对累计256，且无锁泄漏。
- runner 覆盖 A / E / E-skew / Payment / NewOrder × 三等待策略 × 三种本地配置，共45个小型 case；均32线程、关闭协程。
- RDMA 回环与跨主机各18项：两个协议 × 三等待策略 ×（Lp/Lu contract、两个独立进程对同两 key 各累加15次且最终精确为30、共享YCSB-E）。
- 首轮 RDMA 原始日志：experiments/logs/precision-check-20261108-141911-083247（回环）、precision-check-20261108-142000-983995（跨主机）。这两轮代码处于未提交状态；state.json 保存基线HEAD、已跟踪 diff 指纹和实际二进制SHA256。新增未跟踪源码不在 git diff 中，不能把该指纹称作完整源码快照；实际二进制已固定。
- 完整 CTest、sanitizer 与一小时连续验收的最终结果见本文件后续验收补记；未完成前不宣称全部达到仓库完成标准。
- 正式矩阵和新协议的性能指标采集尚未启动；本次交付为论文机制实现与正确性验收，不声称已有正式性能结果。

### 最终验收补记（持续更新至提交前）

- Release 完整 CTest：61项，40项实际执行通过，21项因缺少专用测试环境变量按既有规则跳过，零失败。日志 /tmp/rrl-precision-ctest-final.log。
- Debug + AddressSanitizer / UBSan：新增5项全部通过（含45个runner组合），无 sanitizer 错误。全新构建目录 /tmp/rrl-precision-asan，日志 /tmp/rrl-precision-asan-test.log。
- ThreadSanitizer：4项并发专项全部通过，无数据竞争报告；本地精确累计测试另在 ASan/UBSan 下通过。构建 /tmp/rrl-precision-tsan，日志 /tmp/rrl-precision-tsan-test.log、/tmp/rrl-precision-asan-sum.log。
- 最新回环18/18通过：experiments/logs/precision-check-20261108-143351-697468。
- 最新跨主机18/18通过：experiments/logs/precision-check-20261108-143400-969377。
- 附加全热点探针发现：32个写者同时先持有相同 Lp 再请求 Lu 时，no-wait 可反复互相中止，有限重试内未必有提交。这不证明数据不一致，也不允许宣称无饥饿；精确累计验收采用16对有两个竞争者的键，保留全部提交和精确结果断言，不用 sleep/退避修饰协议。
- 连续一小时结构压力验收 PASS：32线程，12828065次成员修改，272432次成功扫描，最终8221个键与参考集合完全一致。同步滞后拒绝159346727次，容量拒绝1264125060次，是结构压力探针的操作计数，不是正式事务中止率。日志 /tmp/rrl-griffin-soak.log。
