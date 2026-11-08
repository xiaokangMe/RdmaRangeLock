# Precision Locks 与 Griffin 复现方案（2026-11-08）

## 文献与源码边界
- Jordan/Banerjee/Batman, Precision Locks, SIGMOD 1981, pp.143–147，§4.1–4.3。已阅读用户提供的五页全文。
- Nakazono/Bessho/Kawashima/Nakamori, Griffin: Fast Transactional Database Index with Hash and B+-Tree, arXiv:2407.13294，§II-C2、§III、算法1–2、§IV。已阅读十页全文。
- 搜索公开网页及 GitHub 仓库，没有找到可直接移植的这两项完整实现；Griffin 引用 OpenBwTree 与 LineairDB，但不是其自身代码。按原文算法独立实现，无第三方依赖。
- Griffin-PPL 只有题录，不据摘要猜测 PPL 的去中心化链表算法，不把本实现标作 PPL。

## 统一架构中的两个锁协议
1. precision-lock：Lp 保存读谓词，Lu 保存更新点，只进行 Lp × Lu 的谓词求值。所有写操作（含唯一键插入、删除、upsert）先登记点读谓词，再登记实际更新，按 Jordan §4.1 隐含读的要求排除写写异常。当前谓词语言限单个 uint64 键区间，不宣称支持任意元组谓词。
2. griffin-pl：复现 Griffin 的 Insert/Delete × Scan 防幻读部件；点读写另用统一 record 2PL，范围扫描先登记 Lp，再对扫描结果加 record S 并读取值，防止原论文未覆盖的多操作事务值异常。Update 发现缺失键时按插入登记 Lu。
3. 两者均支持本地和现有原生 RDMA 锁槽后端；沿用三种等待策略、事务状态、提交仲裁与 strict 2PL。Lp/Lu 的检查和发布在同一组分片临界区中原子完成，等价于论文发布后检查，不引入读写同时漏检的窗口。槽种类使用附加枚举值，旧值不变。

## Griffin 混合索引
新增本地 hash 点访问 + 现有 B+树键范围访问 + 单独 sync manager。已提交的成员变动进入 FIFO 同步队列，在树同步完成以前，pending 元数据持续拒绝相关扫描，等效于论文保留 committed Lu 的可见性保护。事务私有写集直到提交前不进入 hash，因而 abort 无需物理回滚（统一骨架适配）；range query 的值从 hash 获取。不要把一个 B+树加 PL 冒充完整 Griffin 索引。
远端现有 index=rdma 可测试 griffin-pl 的锁机制，但不宣称它是远端 hash+B+tree 混合索引。本地 index=griffin 与该锁协议必须成对使用，禁止跨客户端依赖本地 hash 副本。

## 正确性验证
- Lu/Lp 两方向、读读/更新更新不比较、唯一插入与修改的隐含读、端点与空范围、并发发布只有一个冲突方可通过、提交前持锁。
- 多操作事务、读己之写、缺失 Update、Delete、反复同键更新；三种等待策略。
- Griffin pending 阻挡扫描、同步后放行、FIFO 删除再插入、同一批同键多次操作、同步故障 fail closed、abort 不污染索引。
- CTest、实际 RDMA 回环以及可用时 .66/.67 小规模正确性验收；不启动正式性能矩阵。
