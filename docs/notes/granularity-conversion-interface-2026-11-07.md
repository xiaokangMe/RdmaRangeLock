# 粒度转换可选接口（2026-11-07）

新增 ILockModeConversion::convert_lock，IRangeLockManager 原有签名不变。本地 LockManager 和远端 RemoteLockManager 支持现有 bounded Node 的原位模式替换；不支持凭空创建、跨资源替换或无穷根转换。不同于 lock 的 permission join，convert_lock 按指定模式替换。

同一桶覆盖的全部分片 mutex / RDMA gate 在验证到发布期间持有；先检查新模式对其他事务的冲突，普通失败不修改旧模式。远端发布完成后 exact replace 缓存模式，不能继续缓存已被降级的 X。硬错误使运行失败，不能当普通冲突继续。接口本身不证明降级安全，由事务层先恢复全部历史记录和 phantom 谓词；禁止先解锁再恢复。

HierarchyPredicate 为固定资源树的后代一维谓词保护，纯父意向不与它直接冲突，实际点写 Record X 按几何相交排斥。用于把 bucket 额外过锁替换成连续的原始扫描保护，不是分离 gap guard。

语义版本：range region5、TCP RRLWAIT4；object 仍为 RRLOBJ02（其精确对象语义未变）。旧服务须重新启动，未操作用户现存服务。

验证：构建及真实回环完整 CTest49/49，无跳过，日志 experiments/logs/classic-mgl-check-20261107-083205/ctest.log。新增本地/远端共同交错：升级冲突保留 IX、升级后删除冗余记录仍有保护、恢复历史后降级、缓存不保留旧 X、空结果 phantom 范围在降级后仍排斥插入、范围外写入恢复并发。运行时阈值和工作负载接入是后续独立里程碑。
