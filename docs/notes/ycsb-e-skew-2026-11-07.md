# YCSB-E-skew 参数与实现依据（2026-11-07）

## 完整计划：A、E、E-skew 都保留

此前五组是 E-skew 的分布对照，不是全部负载。完整第一轮包含两组基线与五组 E-skew：

| 负载/组 | 读 | 更新 | scan | insert | 请求分布 |
|---|---:|---:|---:|---:|---|
| YCSB-A | 50% | 50% | 0 | 0 | Zipf theta=0.4 |
| YCSB-E | 0 | 0 | 95% | 5% | Zipf theta=0.4 |
| E-skew uniform 对照 | 0 | 0 | 95% | 5% | uniform |
| E-skew 80/20 | 0 | 0 | 95% | 5% | hotspot p=0.8、f=0.2 |
| E-skew 80/10 | 0 | 0 | 95% | 5% | hotspot p=0.8、f=0.1 |
| E-skew 90/20 | 0 | 0 | 95% | 5% | hotspot p=0.9、f=0.2 |
| E-skew 90/10 | 0 | 0 | 95% | 5% | hotspot p=0.9、f=0.1 |

所有组固定 32 线程、execution_mode=threads、coroutines=1、2^21 键、10 请求/事务、10 秒预热、10 秒测量、单次运行（种子 8）。A 不包含 scan，矩阵运行时 range=0；E/E-skew 固定 range=32。基础配置中的 ranges=[32] 对 A 不展开扫描维度。

已补齐两个基线文件（同一目录 experiments/configs/ycsb_e_skew/）：
- ycsb-a-zipf-0p4.json
- ycsb-e-zipf-0p4.json

两个基线加五个 E-skew，一共 7 组；本地 133 cases、RDMA 126 cases，总计 259 主 cases，门禁和容量预检另计。此为配置计数，不是已完成的实验。均匀组用于解释热点影响；不能声称 E 默认均匀。

若后续正式考察“Zipf 偏斜率”，A 与 E 使用同一组 theta 参数，按用户指定四档 0、0.4、0.7、0.9。这是第二阶段扩展，当前补齐的两个基线均固定 theta=0.4，未自动扩成多档或启动实验。

## 本轮仅测六种不同协议，区间锁统一选 exact-range

用户选择 exact-range。当前 predicate 与 exact-range 在统一事务执行器中采用相同的区间谓词保护：scan 取区间 S、写入取目标点 X，使用相同冲突机制；PolicyProtocol/LockKind 标签不同，不作为两个独立算法计数或解释性能差异。

本轮配置仅保留 record-2pl、gap-lock、next-key、prior-key、hierarchical（bucket）、exact-range，本地另保留 none 基线。predicate 的本地/RDMA实现和既有正确性覆盖保留，历史配置/结果不删除不重标。

三种等待策略全部展开：no-wait、wait-die、wound-wait；none 不重复三种策略，只执行一次。每项 case 只测一次，seed=8；这里的“重试次数”仍由事务冲突处理产生，不等于独立实验重复次数。

## E-skew 的五组分布配置

用户指定：只给出配置，不启动实验；正式配置固定 32 个 worker 线程，execution_mode=threads、coroutines=1，不启用协程。
推荐先比较 5 组。X/Y 统一表示 X% 请求访问 Y% 的数据，禁止倒置。

| 组 | request_distribution | hotspot_data_fraction | hotspot_opn_fraction |
|---|---|---:|---:|
| uniform | uniform | 不生效 | 不生效 |
| 80/20 | hotspot | 0.20 | 0.80 |
| 80/10 | hotspot | 0.10 | 0.80 |
| 90/20 | hotspot | 0.20 | 0.90 |
| 90/10 | hotspot | 0.10 | 0.90 |

固定 20% 或 10% 热点数据，比较 80% 与 90% 请求，衡量请求集中程度；固定 80% 或 90% 请求，比较 20% 与 10% 热点数据，衡量热点大小。四个热点构成 2×2 因子设计；每项因果比较只改变一个参数。均匀组是独立对照，不把四个热点组连成“单一偏斜率”曲线。

共享参数：

| 参数 | 配置 | 含义 |
|---|---|---|
| workloads | [ycsb-e-skew] | E 操作配比，显式记录分布 |
| keys | 2097152 | 2^21 的键域规模，约 210 万 |
| operations | 10 | 单事务 10 个请求，scan 是一个请求 |
| ranges | [32] | 固定半开键区间宽度，不是精确返回 32 行 |
| threads | [32] | 32 个线程 |
| execution_mode / coroutines | threads / 1 | 普通线程路径，不启动协程 |
| warmup_seconds / duration_seconds | 10 / 10 | 全并发预热、正式测量窗口 |
| runs / seeds | 1 / [8] | 每个 case 测一次 |
| wait_policies | [no-wait,wait-die,wound-wait] | 每个负载/算法分别比较三种等待策略 |
| hierarchy_layout / buckets | bucket / [64] | 固定层级桶几何，升降粒度关闭 |
| coordination_backend | native-rdma-state | 原生单边远端状态、数据、锁路径 |
| native_object_locks | 1 | RDMA record 使用原生对象锁 |
| rdma_lock_slots_per_shard | 32768 | 候选远端区间锁容量；正式执行前仍必须通过容量预检 |
| tree_pages / tree_records | 131072 / 4194304 | 两百万键规模的共享树容量 |
| ycsb_unique_keys | 1 | 保留项目的事务内 anchor 去重规则 |

操作配比是生成请求的期望 95% scan / 5% insert，不保证每个 10 请求事务恰好 9.5 次扫描。比例不是已提交事务中的操作比例。

## 名称与标准边界

官方 YCSB 的 Workload E 本身使用 requestdistribution=zipfian、95% scan / 5% insert、maxscanlength=100、scanlengthdistribution=uniform。因此“标准 E=均匀，标准 E-skew=热点”不是官方定义。未发现跨论文统一规定、具有唯一参数的 E-skew 第七种标准负载。

本实现采用 **官方 E 操作配比 + 官方 hotspot 采样模型**，在项目统一事务架构中运行。实验应命名为“YCSB-E 操作配比的事务化 hotspot 扩展”，并披露以下适配：

- 数值键、有序键空间；热点位于连续低键区间 [0,H)，H=floor(keys×hotspot_data_fraction)。不是 hashed insertorder 下分散的热点。
- 按 hotspot_opn_fraction 概率选择热点，随后在热点/冷点集合内均匀抽样；与官方 HotspotIntegerGenerator 的集合选择模型一致。空集合且选择概率非零时拒绝配置，不继承上游静默修正或除零行为。
- 本项目 scan 和 insert 的 anchor 都使用该分布。官方 CoreWorkload 的 INSERT 使用新增键序列，requestdistribution 主要控制已有记录访问，二者不能声称逐请求完全相同。
- 保留固定键宽 [k,min(keys,k+range_length))、事务内 anchor 去重、有限键域插入及时间窗口内循环执行 trace。不是原版按返回记录数扫描、持续扩张键域的完整 Java CoreWorkload。
- 预热会改变数据存在性，有限键域反复插入可能成为已有键覆盖。不能把全部 Insert 标签解释成新增记录，更不能据此声称完整复现插入增长行为。
- 同事务 anchor 去重会轻微改变接受请求的分布；热点比例定义作用于候选抽样，scan 越过热点边界也会访问冷区。实际行访问比例和提交后的比例不等于配置概率。

旧 ycsb-e 的默认 theta=0.99 及随机序列不变，不回填或重标旧数据；显式选择相同 zipfian 参数时 E-skew 与旧 E 的 trace 一致。

## 参数接口

runner CLI：

~~~text
--workload ycsb-e-skew
--request-distribution hotspot
--hotspot-data-fraction 0.2
--hotspot-opn-fraction 0.8
--threads 32 --execution-mode threads --coroutines 1
~~~

以上为参数片段，不是本次已运行的命令。

runner / 共享数据与对象锁服务 JSON 支持：

~~~json
{
  "workload": "ycsb-e-skew",
  "request_distribution": "hotspot",
  "hotspot_data_fraction": 0.2,
  "hotspot_opn_fraction": 0.8,
  "key_count": 2097152,
  "transaction_count": 5000,
  "operations_per_transaction": 10,
  "range_length": 32,
  "threads": 32,
  "execution_mode": "threads",
  "coroutines": 1,
  "warmup_seconds": 10,
  "duration_seconds": 10,
  "seed": 8,
  "zipf": 0
}
~~~

同一对象支持官方参数名 requestdistribution、hotspotdatafraction、hotspotopnfraction；同字段的两个别名不能重复出现。runner 配置（workload/threads 标量）与 campaign 配置（workloads/threads 数组）不是同一个 JSON schema，不能互换。

request_distribution 允许 auto、uniform、zipfian、hotspot。auto 对 E-skew 默认 hotspot，对旧负载保持原有 Zipf/uniform 行为。两个热点比例均要求有限且在 [0,1] 内；抽样区域不能为空；如果仅访问一个集合，该集合必须容得下单事务唯一 anchor 数。zipf 非负且有限；只对 zipfian/旧 auto 路径生效，hotspot 模式不能用 theta 表示强度。

## 五组配置与生成工具

已准备在 experiments/configs/ycsb_e_skew/：

- ycsb-e-skew-uniform.json
- ycsb-e-skew-hot-ops-0p8-data-0p2.json
- ycsb-e-skew-hot-ops-0p8-data-0p1.json
- ycsb-e-skew-hot-ops-0p9-data-0p2.json
- ycsb-e-skew-hot-ops-0p9-data-0p1.json

每组本地 19 cases（6 种锁×3 策略 + none 一次，32 线程，1 seed）；RDMA 18 cases（6 种锁×3 策略）。五组 E-skew 主矩阵共 185 cases，容量预检和正确性门禁另计。本次没有启动这些 cases。

scripts/make_ycsb_e_skew_configs.py 只生成配置，不建链、不构建、不运行实验；默认固定 32 线程且关闭协程，不覆盖已有文件。示例：

~~~sh
python3 scripts/make_ycsb_e_skew_configs.py --out /tmp/ycsb-e-hotspot-plan
~~~

后续 A 与 E 单独比较 Zipf，按用户指定四档：0、0.4、0.7、0.9。
Q-Store Figure 6 的七档仅作为文献背景，当前配置不采用其档位。这是参数设计参考，不是复制 Q-Store 的分区和事务设置；它在分区内偏斜，分区选择均匀，本系统是整个键域采样。

~~~sh
python3 scripts/make_ycsb_e_skew_configs.py --distribution zipfian --out /tmp/ycsb-e-zipf-plan
~~~

Zipf 与 hotspot 必须分开报告；theta=0.99 不等于“99% 请求访问 1% 数据”。同一等待策略内比较分布，同一分布内比较等待策略；第一轮不同时扩大 scan 长度、线程数、协程数或桶宽；以后再选代表分布补 8/32/128。

## 实现与可追溯性

WorkloadKind 末尾追加 YcsbESkew，保持旧枚举值；不改变锁管理器签名、页布局、事务执行骨架或 RDMA 通信协议。scan/insert 执行复用既有 E 路径。

runner CSV 增加 trace_* 字段：theta、分布选择/实际分布、热点两个比例、keys、trace 事务数、单事务请求数、range、seed、唯一键规则、扫描语义。矩阵执行、续跑、报告都会核验参数；不将不同配置当成重复，也不允许缺乏 trace 参数的 E-skew 结果进入报告。历史 E CSV 仍按旧规则读取，不自动变成 E-skew。

报告图题和 summary.csv 标明实际分布及参数；每种分布/比例使用独立 campaign 配置指纹。七种策略的本地/RDMA共享采样器和 seed 构造。

## 验证及未执行事项

此前负载接入版本已完成 build-shared 构建及完整 CTest **53/53 通过，无跳过**，包括：
- 新旧 E 在显式相同 Zipf 参数下 trace 相同；
- hotspot 的 80/20、90/20、80/10、90/10 等分布频率与请求种类检查、合法端点和非法配置拒绝；
- 固定 seed、同事务唯一 anchor、本地/远端初始数据一致；
- 七种策略在本地和真实 RDMA 回环使用 E-skew 小负载，检查远端数据流量；
- 官方参数别名、本地矩阵执行/续跑/报告门禁；配置生成器固定 32 线程、普通线程路径。
- 新 E-skew 小负载断言 execution_mode=threads、coroutines=1、coroutine_switches_total=0。

日志：experiments/logs/classic-mgl-check-20261107-091739/ctest.log。该结果不覆盖本轮单次测量脚本改动，本轮未运行构建或 CTest。
正确性回环不是 .66/.67 跨主机性能结果。**五组正式配置未运行、没有新性能数字**。最后“别跑”要求后仅整理文档/配置和提交，不再执行测试或负载。

## 七种算法的插入路径（源码核对）

七种协议均支持本地与原生 RDMA Insert；none 无锁对照也能写入。A 的默认 50/50 读更新不会主动生成 Insert；E/E-skew 才是本次插入路径的负载入口。

| 算法 | 插入时的事务保护 |
|---|---|
| record-2PL | 插入目标 Record X；支持插入，但不保护范围扫描的 gap，因此不防幻读 |
| gap-lock | InsertIntent + Record X，与已有 gap 抑制保护冲突 |
| next-key | 用实际物理邻键确定 next-key 区间并取 X，锁后重新验证邻键 |
| prior-key | 用实际物理邻键确定镜像 prior-key 区间并取 X，锁后重新验证邻键 |
| 层级桶锁 | 祖先/桶 IX（混合 scan 的桶用 SIX）+ Record X；插入与桶 S 冲突 |
| predicate | 插入目标 point X，与覆盖此点的扫描 Predicate S 冲突 |
| exact-range | 插入目标 point X，与覆盖此点的扫描 ExactRange S 冲突 |

写操作统一进入 src/txn/policy_workload.cpp 的 pending 集合，prepare 成功后 index.apply_batch(pending)。本地默认实现调用 BTreeIndex::insert；RDMA 使用 BTreeRdma::apply_batch，通过远端树/记录写入路径。不能把“有 Insert 分支”等同于全量持续新增压力已经验证。

其他层级布局也有插入路径：真实 leaf 模式规划记录 X、叶/安全祖先 X 处理结构变化；DAG 写入取得所有父路径意向锁和共享 Record X。它们不是本轮固定 bucket 的额外算法组。显式 Deneva 兼容早释放模式是独立非 strict 例外，可能在分裂分支 Abort，本轮使用 strict，不混组。

有限键域与预热的限制仍在：Insert 可能针对已存在键并覆盖；5% Insert 请求不等于 5% 真正新建记录。当前配置不能据此声称测量了持续键域增长、所有插入均引发新记录创建，或新键密集分裂路径的全部性能。本轮为源码检查，未新跑插入负载。

## 阅读依据

1. [官方 workloade](https://github.com/brianfrankcooper/YCSB/blob/master/workloads/workloade)：95/5 与默认 zipfian、scan length 选项。
2. [官方 HotspotIntegerGenerator](https://github.com/brianfrankcooper/YCSB/blob/master/core/src/main/java/site/ycsb/generator/HotspotIntegerGenerator.java)：冷热分集合、集合内均匀，热点 rank 连续。
3. [官方 CoreWorkload](https://github.com/brianfrankcooper/YCSB/blob/master/core/src/main/java/site/ycsb/workloads/CoreWorkload.java)：默认热点数据/请求比例 0.2/0.8；buildKeyName、doTransactionScan、doTransactionInsert 的区别。
4. [Albatross，PVLDB 2011](https://www.vldb.org/pvldb/vol4/p494-das.pdf)：附录 Figure 8，论文 p.504 / PDF p.11，明确对比 90/10、90/20、80/10、80/20；本项目只引用访问分布，不宣称复现其迁移系统或 E 配比。
5. [Q-Store，EDBT 2020](https://openproceedings.org/2020/conf/edbt/paper_39.pdf)：第 6.1 节 Figure 6，论文 p.80 / PDF p.8，Zipf 7 档；注意其分区内偏斜和全局偏斜不同。
6. [Cao 等，FAST20](https://www.usenix.org/system/files/fast20-cao_zhichao.pdf)：第 7.1–7.3 节，论文 pp.218–219 / PDF pp.11–12，区分整体 key hotness 与 key-space locality，分别比较 uniform/Zipfian/hotspot/exponential；不能用一个 skew 数字替代热点布局。
7. [ZKV，TACO](https://ranger.uta.edu/~jiang/publication/Journals/2025/ACM-TACO%28ZKV%29.pdf)：第 4.1 节 Table 2 / PDF p.15，Skew-e=S95 U5，YCSB-e=S95 W5；论文只写 80% hotspot，未完整说明热点数据比例，不补造参数。第 4.2 节指出 skew scan 会受锁冲突影响。

相关 PDF 已下载本机 zongshu/papers；官方三个源文件快照保存在 papers/ycsb-source，查询日期 2026-11-07。源文件 SHA256：
- workloade：0e75eea21df10bc1304f25c8daa6e16c2bfbfcb83f9b5a4d794260f470cf7dd1
- HotspotIntegerGenerator.java：9f230d5d22be0338ce0196751b57206d4bcd524f3fd96b110c4305a68eb5f6c2
- CoreWorkload.java：b872f2c2e585e90d56cd9f08e050f26c18ee1d834c26f537793fa4df4258bbc1

## 本轮修订与验证状态

用户要求单次测量、三种等待策略、A/E 默认 theta=0.4，后续 theta=0/0.4/0.7/0.9，不启动实验。
配置新增 repetition_policy=single-run；runner 与 report 只对显式该标记允许 runs=1，保留旧 repeated 默认 >=3 的门禁，保留 >=10 秒要求。单次报告不画运行间波动误差线，不宣称重复实验或置信区间。
配置生成器同步默认值；A/E 基线文件名与 theta 一致。用户本次要求仅准备配置：不运行负载、构建或 CTest，仅做静态语法/配置核对；之前 53/53 结果不覆盖本轮脚本改动。
