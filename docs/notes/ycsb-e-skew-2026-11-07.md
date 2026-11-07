# YCSB-E-skew 参数与实现依据（2026-11-07）

## 完整计划：A、E、E-skew 都保留

此前五组是 E-skew 的分布对照，不是全部负载。完整第一轮包含两组基线与五组 E-skew：

| 负载/组 | 读 | 更新 | scan | insert | 请求分布 |
|---|---:|---:|---:|---:|---|
| YCSB-A | 50% | 50% | 0 | 0 | Zipf theta=0.99 |
| YCSB-E | 0 | 0 | 95% | 5% | Zipf theta=0.99 |
| E-skew uniform 对照 | 0 | 0 | 95% | 5% | uniform |
| E-skew 80/20 | 0 | 0 | 95% | 5% | hotspot p=0.8、f=0.2 |
| E-skew 80/10 | 0 | 0 | 95% | 5% | hotspot p=0.8、f=0.1 |
| E-skew 90/20 | 0 | 0 | 95% | 5% | hotspot p=0.9、f=0.2 |
| E-skew 90/10 | 0 | 0 | 95% | 5% | hotspot p=0.9、f=0.1 |

所有组固定 32 线程、execution_mode=threads、coroutines=1、2^21 键、10 请求/事务、10 秒预热、10 秒测量、3 个种子。A 不包含 scan，矩阵运行时 range=0；E/E-skew 固定 range=32。基础配置中的 ranges=[32] 对 A 不展开扫描维度。

已补齐两个基线文件（同一目录 experiments/configs/ycsb_e_skew/）：
- ycsb-a-zipf-0p99.json
- ycsb-e-zipf-0p99.json

两个基线加五个 E-skew，一共 7 组；本地 168 cases、RDMA 147 cases，总计 315 主 cases，门禁和容量预检另计。此为配置计数，不是已完成的实验。均匀组用于解释热点影响；不能声称 E 默认均匀。

若后续正式考察“Zipf 偏斜率”，A 与 E 使用同一组 theta 参数，建议五档 0、0.6、0.8、0.9、0.99（更完整的七档加 0.3、0.95）。这是第二阶段扩展，当前补齐的两个基线均固定 theta=0.99，未自动扩成多档或启动实验。

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
| runs / seeds | 3 / [8,9,10] | 3 个固定随机种子 |
| wait_policies | [no-wait] | 第一轮固定等待策略，避免和分布同时变化 |
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

每组本地 24 cases（7 种锁 + none，32 线程，3 seed）；RDMA 21 cases（7 种锁）。五组主矩阵共 225 cases，容量预检和正确性门禁另计。本次没有启动这些 cases。

scripts/make_ycsb_e_skew_configs.py 只生成配置，不建链、不构建、不运行实验；默认固定 32 线程且关闭协程，不覆盖已有文件。示例：

~~~sh
python3 scripts/make_ycsb_e_skew_configs.py --out /tmp/ycsb-e-hotspot-plan
~~~

如后续单独比较 Zipf，推荐 Q-Store Figure 6 的 7 档：
0、0.3、0.6、0.8、0.9、0.95、0.99。
精简 5 档可选 0、0.6、0.8、0.9、0.99。这是参数设计参考，不是复制 Q-Store 的分区和事务设置；它在分区内偏斜，分区选择均匀，本系统是整个键域采样。

~~~sh
python3 scripts/make_ycsb_e_skew_configs.py --distribution zipfian --out /tmp/ycsb-e-zipf-plan
~~~

Zipf 与 hotspot 必须分开报告；theta=0.99 不等于“99% 请求访问 1% 数据”。第一轮不要同时扩大 scan 长度、等待策略、线程数、协程数或桶宽；以后再选代表分布补 8/32/128。

## 实现与可追溯性

WorkloadKind 末尾追加 YcsbESkew，保持旧枚举值；不改变锁管理器签名、页布局、事务执行骨架或 RDMA 通信协议。scan/insert 执行复用既有 E 路径。

runner CSV 增加 trace_* 字段：theta、分布选择/实际分布、热点两个比例、keys、trace 事务数、单事务请求数、range、seed、唯一键规则、扫描语义。矩阵执行、续跑、报告都会核验参数；不将不同配置当成重复，也不允许缺乏 trace 参数的 E-skew 结果进入报告。历史 E CSV 仍按旧规则读取，不自动变成 E-skew。

报告图题和 summary.csv 标明实际分布及参数；每种分布/比例使用独立 campaign 配置指纹。七种策略的本地/RDMA共享采样器和 seed 构造。

## 验证及未执行事项

在用户最后要求“先给参数别跑”之前，已完成 build-shared 构建及完整 CTest **53/53 通过，无跳过**，包括：
- 新旧 E 在显式相同 Zipf 参数下 trace 相同；
- hotspot 的 80/20、90/20、80/10、90/10 等分布频率与请求种类检查、合法端点和非法配置拒绝；
- 固定 seed、同事务唯一 anchor、本地/远端初始数据一致；
- 七种策略在本地和真实 RDMA 回环使用 E-skew 小负载，检查远端数据流量；
- 官方参数别名、本地矩阵执行/续跑/报告门禁；配置生成器固定 32 线程、普通线程路径。
- 新 E-skew 小负载断言 execution_mode=threads、coroutines=1、coroutine_switches_total=0。

日志：experiments/logs/classic-mgl-check-20261107-091739/ctest.log。
正确性回环不是 .66/.67 跨主机性能结果。**五组正式配置未运行、没有新性能数字**。最后“别跑”要求后仅整理文档/配置和提交，不再执行测试或负载。

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
