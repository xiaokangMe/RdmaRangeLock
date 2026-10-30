# RdmaRangeLock

范围查询与范围锁的综述研究 + 最小可复现实现集。

本仓库同时承担两件事：

1. **一篇综述文章**：系统梳理范围查询在并发控制中的语义问题，以及 next-key lock、gap lock、层级锁、latch coupling 等方案的演化关系。
2. **一套最小实现**：用同一棵 B+ 树、同一套 workload，逐个叠加锁机制，得到可对比、可复现的实验数据。

核心主张：**综述不能只靠讲故事**。每一类锁机制都要有一份能跑的实现，用同一组 workload 测出差异，文章才有说服力。

---

## 一、六个实现版本（路线图）

| # | 名称 | 目录 | 核心机制 | 关键不变式 | 主要对比指标 |
|---|---|---|---|---|---|
| v1 | 单线程 B+树 | `src/index/` | 无并发控制 | 树结构始终有效 | 基线：吞吐、树高、点查/范围查延迟 |
| v2 | 并发 B+树 + Latch Coupling | `src/index/` | crabbing（分段加锁、父子 latch 交替） | latch 顺序自顶向下，无死锁 | 多核扩展性、latch 争用率 |
| v3 | B+树 + Record-Level Strict 2PL | `src/lock/` | 仅锁记录，不加 gap | 已提交读/可重复读语义 | 幻读发生率、并发度 |
| v4 | B+树 + Gap Lock | `src/lock/` | 记录锁 + 间隙锁 | 范围谓词稳定 | 锁数量、阻塞率、幻读是否消除 |
| v5 | B+树 + Next-Key Lock | `src/lock/` | 记录锁与间隙锁合并为一个区间锁 | 区间 (prev, curr] 互斥 | 与 v4 的锁开销/正确性差异 |
| v6 | B+树 + Next-Key Lock + Hierarchical Locking | `src/lock/` | 在 next-key 之上引入多粒度意向锁（IS/IX/S/X） | 意向锁沿树路径向上传播 | 大范围扫描的锁开销下降幅度 |

**递进关系**：v1→v2 解决"能不能并发"，v2→v3 解决"事务语义"，v3→v4→v5 解决"范围谓词的幻读"，v5→v6 解决"大范围锁的代价"。

这张表就是文章的骨架：每一行对应论文章节里的一节，每一节的结论都要有 v(N) 与 v(N-1) 的实测数据支撑。

---

## 二、目录约定

沿用本机既有系统类项目（deneva / Writeflex）的组织习惯：**按子系统分目录，每类协议独立文件，benchmark 与统计、画图脚本分离**。

```
RdmaRangeLock/
├── include/            公共头文件，与 src/ 镜像对应
│   ├── common/         基础类型、配置、日志、时间戳
│   ├── index/          B+树接口（页布局、节点、迭代器）
│   └── lock/           锁管理器接口（lock mode、lock table、hierarchy）
├── src/
│   ├── common/         工具实现
│   ├── index/          v1 单线程 B+树、v2 latch coupling
│   ├── lock/           v3 record 2PL、v4 gap lock、v5 next-key、v6 + hierarchical
│   └── txn/            事务骨架、调度器、workload 驱动（各版本共用）
├── benchmarks/         负载与数据
│   ├── schema/         表结构定义
│   └── workload/       点查 / 范围查 / 混合读写 / 幻读构造用例
├── experiments/        实验配置与产出
│   ├── configs/        每个实验一份配置（版本、并发度、workload 参数）
│   ├── results/        汇总后的 csv/json（原始大文件不入库）
│   └── logs/           运行日志
├── statistics/         运行时统计采集（锁等待、latch 争用、重试次数）
├── scripts/            构建、批量跑实验、出图
├── docs/
│   ├── survey/         综述正文（按章节拆分，便于逐章迭代）
│   ├── refs/           参考文献
│   └── notes/          阅读笔记、设计讨论、待验证猜想
├── figures/            成品图（脚本生成，不放手工截图）
├── test/               单元测试与正确性验证（幻读检测用例放这里）
└── thirdparty/         第三方依赖（若无则留空）
```

---

## 三、写作与实现的对应关系

文章的每一节都要能指回仓库里的具体文件：

- 讲 next-key lock 的定义 → `src/lock/v5_next_key_lock.cpp` 是它的可执行定义
- 说"gap lock 的锁数量是 next-key 的两倍" → `experiments/results/` 里有对应 csv
- 说"层级锁把大范围扫描的锁开销降下来" → `figures/` 里有 v5/v6 的对比图

**没有对应实现的论断，不写进文章。**

---

## 四、里程碑

- [x] M1 接口与区间锁表示：完成 deneva 风格类型、显式区间和 LockManager，Debug/Release 均通过
- [ ] M2 v1 正确性：单线程 B+树通过插入/删除/范围扫描测试
- [ ] M3 v2 并发：latch coupling 在 64 核下无死锁、无结构破坏（压力测试 1 小时）
- [ ] M4 v3~v5 语义：三个版本跑同一组幻读用例，结果可解释
- [ ] M5 v6 层级锁：意向锁传播正确，大范围扫描锁数量下降可量化
- [ ] M6 综述初稿：分类学表格 + 六组对比数据齐备

---

## 五、工程约定

- **一个版本一次提交**，提交信息写明实现了哪个不变式
- 任何性能结论必须附 `experiments/configs/` 里的配置文件与 `results/` 里的原始数据
- 图必须由 `scripts/` 下的脚本生成，`figures/` 只存产物
- 原始数据集、编译产物、运行日志不入库（见 `.gitignore`）

---

代码层面的硬约束见 [CONSTRAINTS.md](CONSTRAINTS.md)；违反其中任何一条，对应提交直接回退。
