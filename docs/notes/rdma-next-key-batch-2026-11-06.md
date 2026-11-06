# RDMA next-key 批量授权（2026-11-06）

## 改动

`RemoteLockManager` 新增独立能力接口 `IRangeLockBatch`，不改变 `IRangeLockManager` 的六版公共签名。远端 next-key 的一次范围读会先扫描得到完整 next-key 区间集合，再执行：

```text
并集合并涉及的 shard
→ 一次 CAS 获取 gates
→ 一次读取活动槽前缀
→ 本地检查全部区间冲突
→ 一次批量发布所有新槽/升级
→ 释放 gates
```

只有远端管理器实现该能力；本地 `LockManager` 和旧兼容路径仍逐锁执行，因此协议语义可以直接对照。批量调用只在所有区间通过冲突和容量检查后发布，任一冲突不会留下部分 next-key 锁。事务结束仍由原有 `unlock_all` 统一释放，保持 strict 2PL。

## 覆盖范围

该批量能力只用于 `run_next_key_lock` 的单个范围读。点读、写、Gap、Predicate 和层级协议暂不改变；没有把区间锁误换成对象锁。跨 shard 的区间会按实际 shard 集合发布，不会复制到无关 shard。

## 验证

增加 RDMA 客户端回归：两条跨 shard next-key 区间一次批量发布、冲突事务被拒绝、释放后重新获取成功。当前环境未提供 RDMA 设备，因此该硬件测试按既有条件跳过；编译与 CTest 45/45 通过。
