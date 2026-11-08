# 真实 B+ 树叶节点多粒度锁复现（2026-11-08）

## 论文依据

本实现依据 Gray et al. (1976) 的多粒度锁协议和 Graefe (2010) 对 B-tree 结构锁与事务锁分离的说明。Deneva 源码只作为 RDMA 叶/父对象 CAS 和结构修改路径的工程参考，不把 Deneva 的提前释放语义混入默认 strict 路径。没有找到可直接移植的、完整实现 Gray/Graefe 叶页多粒度事务锁的公开源码，因此在统一项目接口上复现。

## 资源层次

真实资源树为：

    synthetic root topology resource
      -> actual B+ tree internal ancestors
        -> actual B+ tree leaf page
          -> canonical record

叶页和内部节点使用稳定的 B+ tree page identity；事务锁通过 LockKind::Node 传递 point identity，记录使用 LockKind::Record。索引 latch 仍只保护一次结构访问，事务锁仍由统一 IRangeLockManager 管理。

## 锁规则

- 点读：root IS -> internal IS -> leaf IS -> record S。
- 点写：root IX -> internal IX -> leaf IX -> record X。
- 范围读：root IS -> overlapping internal IS -> every covered real leaf S。
- 同一事务范围读和点写落在同一叶页时，leaf 的 S 与 IX 在计划阶段合并为 SIX，避免运行时升级。
- 插入/删除先根据树快照判断可能的 split/merge/root replacement；安全的真实祖先取得 X，叶页取得 X。无法证明祖先安全时取得 synthetic root X。
- 所有锁严格持有到 commit/abort；默认路径不使用 Deneva early-release。

## 结构变化验证

锁获取后重新生成真实祖先/叶页计划：

- 新叶页或新根出现时，旧计划缺少新 page identity，事务中止并重试；
- 删除导致叶页移除、路由重写或根收缩时，当前计划与已持有计划不一致，事务中止并重试；
- 旧对象锁不会因为物理页删除而授权新路径，直到事务结束释放。

## 与固定桶层级的区别

固定桶层级锁定 key / bucket_width 形成的静态区间；本实现锁定真实叶页和真实祖先。两者都使用 Gray 的 IS/IX/S/SIX/X 兼容矩阵，但资源边界、锁数量、树高、split/merge 处理和扩展性统计必须分开。

## 本轮验收

新增 rrl_leaf_mgl_plan_test，覆盖：

- 真实祖先与真实叶页资源；
- 自顶向下 phase；
- 点读/点写的 IS/IX/S/X；
- 范围读的叶页 S；
- 混合访问的 SIX；
- 叶分裂后的 stale-plan 拒绝；
- 删除导致根收缩后的 stale-plan 拒绝。

记录锁阶段使用无符号最大值，保证无论树高如何都在所有祖先之后获取。

## 双机 RDMA 正确性验收（2026-11-08）

在 .66 控制、.67 执行客户端完成 scripts/check_leaf_runtime.py --cross-host --only cross，共 24 项通过：

- strict/root/Deneva/partial/split-abort 祖先与结构修改交错；
- clean/running/committing/gate 四种锁服务断连故障；
- no-wait、wait-die、wound-wait 三种策略；
- YCSB-A、YCSB-E、TPC-C Payment、TPC-C New Order。

这是正确性和通信路径检查，不是正式性能矩阵；正式矩阵数据仍需单独按配置生成。
