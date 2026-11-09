# 扩展锁协议矩阵配置（2026-11-09）

## 配置边界

旧的 `formal_policies_10s.json` 和 `formal_native_policies_10s.json` 保留不动，继续作为历史七入口配置（本地 `none`、record-2PL、gap-lock、next-key、hierarchical，加 RDMA next-key、hierarchical）。本次新增 `experiments/configs/formal_lock_policies_extended_10s.json`，用于新代码基线的统一锁语义比较。

扩展配置加入：

- 本地：`prior-key`、`exact-range`、`precision-lock`、`griffin-pl`；同时显式保留已有 `record-2pl`、`gap-lock`、`next-key`、`hierarchical` 和 `none` 基线。
- 共享 RDMA：`rdma-record-2pl`、`rdma-gap-lock`、`rdma-prior-key`、`rdma-exact-range`、`rdma-precision-lock`、`rdma-griffin-pl`，以及已有的 `rdma-next-key`、`rdma-hierarchical`。

`griffin-pl` 在这个矩阵中固定为 `index=v2`/共享 `index=rdma` 的 Griffin 锁语义消融，不把本地 `index=griffin` 的混合哈希+B+树收益归因给锁协议。`predicate` 保留为代码兼容入口，但不在正式矩阵中单独测量，因为当前实现与 `exact-range` 使用相同的区间交叠语义；`leaf`、`separator`、`dag` 仍然是层级布局消融，也不单列成锁协议。

## 运行维度

两种 YCSB 负载（A、E）、10 秒测量窗口、10 秒 warm-up、3 次重复、1/2/4/8/16/32 线程、三种等待策略（no-wait、wait-die、wound-wait）、范围 8/32/128、桶宽 16/64/256。显式使用 `execution_mode=threads`、`coroutines=1`，防止协程路径改变协议比较。

静态 case 数为 local 2232、cross-host 2160。YCSB-E 额外展开 8/32/128 三个扫描宽度。这个数字只描述展开后的计划，不代表已经跑过实验。

## Citron 单独处理

`experiments/configs/formal_citron_local_10s.json` 只包含本地 Citron，72 个 case。Citron 固定线程执行、`coroutines=1`、`no-wait`，不能套用 wait-die/wound-wait。`rdma-citron` 需要单独启动远端段树锁服务，当前不放进 `run_shared_matrices.py` 的共享数据 RDMA 矩阵；否则会把独立远端锁树路径误标成共享远端 B+树/事务状态路径。

完整跨主机 Citron 如需正式测量，应另写专用 driver，显式记录远端段树服务端、树高、锁槽容量和 `lock-only`/共享数据路径，不能直接复用扩展锁矩阵的 case 计数。

## 验证状态

本次只做静态配置和 runner 白名单/兼容性更新，没有启动正式矩阵；运行前仍需提交源码和配置，并按项目门禁先做预检。
