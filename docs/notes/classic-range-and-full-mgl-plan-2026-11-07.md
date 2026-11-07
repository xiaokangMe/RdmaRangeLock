# 邻接键范围锁、prior-key 与完整多粒度扩展方案

用户授权：在统一架构内修正 gap/next-key 后补 prior-key；桶锁补全模式、粒度升降和 DAG 多路径保护。不得用新增名称替代机制。未授权正式矩阵，本任务只做正确性验证。

## 提交顺序

1. 接口里程碑：索引增加纯结构元数据 key_neighbors（严格前驱/等值存在/严格后继，显式不存在表示无穷），BTreeSingle/BTreeLatch/BTreeRdma 共用契约；IRangeLockManager 原签名不变。模式/槽布局扩展另列接口里程碑，双方版本协商拒绝旧语义。
2. gap/next-key/prior-key：统一事务驱动和旧入口统一路由；本地、RDMA 同一规划。先实现原文的基础算法再谈性能。
3. 完整 MGL：IS/IX/S/X/SIX/U/IU/SIU/UIX 的显式 9×9 矩阵、模式 join、转换检查；桶混合扫描/写用 SIX+记录X，不再升级成整桶X。
4. 粒度转换与 DAG：可选 lock-set 原子替换能力，先发布替代保护再撤销旧保护；失败保持原集合。升/降是表示转换，不是提前放弃已访问数据的保护。DAG 读至少一条合法路径、写全部父路径；跨索引共享 canonical RID，避免仅锁查询选择的一个索引。
5. 定向交错、模型验证、真实 RDMA 回环与完整 CTest；更新出处和配置；不启动正式矩阵。

## 经典范围锁

依据 Graefe2010 §5.1–5.3：基本 next-key (prev,k] / prior-key [k,next)，真实现存键边界，显式±∞；基础 S/X 版本允许比 ARIES/KVL instant/复合模式更保守，明确不称完整 KVL。
gap 选 MySQL8.4 §17.7.1 的纯抑制规则：gap guard 共存；插入意向与覆盖的 guard 冲突，不同位置插入可共存；记录读写仍须 S/X。更新不能绕过记录保护。结构删除后仍保留事务已持数值谓词，不能因边界键消失缩小锁覆盖。

统一实现以稳定数值区间表示相邻键资源。锁名不依赖会消失的物理槽；即使后续键插入/删除，既有锁的逻辑保护集合保持不变，所有新请求按数值相交，因此无需把同一保护复制到另一物理槽。这是对哈希 successor-key 锁名的工程适配，需要单独解释其锁表检索成本，不照搬论文吞吐。

快照→锁→重定位/复扫；等待不持结构 latch；数据从锁后结果读取；own-write overlay 不变；锁保持到提交/中止。不能把 uint64 最大值冒充 +∞。元数据查询 O(树高)，不全表枚举。

## 模式出处与兼容

Gray1976 Table1 与 DAG 规则：IS/IX/S/SIX/X。SQL Server Transaction Locking and Row Versioning Guide 的 intent modes + full lock matrix：U/IU/SIU/UIX；SIU=S+IU，SIX=S+IX，UIX=U+IX。这些是不同出处，统一实验适配不等于全部原始Gray模式。保留旧四模式枚举值，新增模式追加，检查全部 wire/validator/cache/join。
SQL Server 对 IU 的 page-only 限定在本项目映射到中间 bucket 资源，不把 IU 当普通记录读写权限。

## 升降粒度

escalation：细锁集合→同一覆盖桶S/SIX/X，必须检查其他事务意向，失败不得丢细锁。
de-escalation：先恢复完整历史读写/phantom保护和后续操作所需细锁，再把粗锁降为对应意向，失败不能丢粗锁。单纯 unlock(parent) 再 lock(children) 禁止。适配允许释放额外过锁，但已访问谓词/记录的逻辑保护连续到事务结束；若产生实质缩锁则进入 shrinking 且禁止继续取得新保护。
配置必须记录阈值、转换方式和计数，不能静态预规划冒充运行时 escalation。

## DAG

资源标识与数值键区间隔离，拓扑验证拒绝环、未知父节点、孤立节点及溢出。加锁按拓扑层和稳定资源ID排序，模式合并去重。读可沿选定索引路径，写/删除/插入必须覆盖所有索引父路径，并锁同一 canonical record，保护通过另一索引持有的粗S。
当前底座仅有单棵权威键索引，多条逻辑索引锁路径的支持不能宣称已实现不同键投影的完整多索引存储；必须明确协议DAG能力与实际二级索引维护的边界，后者另配负载和存储设计。

## 来源

- Gray1976 PDF pp.4–8：模式、top-down、DAG 读一条路径/写全部父路径。
- Graefe2010 PDF pp.14–19：next/prior、无穷、ghost/SMO、key/gap 分离。
- MySQL8.4 §17.7.1 Gap/Next-Key/Insert Intention。
- Microsoft Learn Transaction Locking and Row Versioning Guide：完整矩阵图片 sql-server-lock-conflict-compatibility.png，IU/SIU/UIX定义；官方图已核对，不用非官方矩阵猜测。

## 接口实现时发现的旧结构错误

随机插入/删除模型在 BTreeSingle 第424步（key=66）触发 validate 失败。旧 refresh_separators 跳过空叶但保留子节点与旧 separator，破坏邻居导航与有序子树边界。现修正为移除空子树、修复叶链、重建分隔键、收缩空/单子根；保留原断言并增加删空树回归。BTreeLatch 原删除路径已维护这些结构。

接口里程碑验证：真实 RDMA 回环完整 CTest 49/49 通过，无跳过；日志 experiments/logs/classic-mgl-check-20261107-080606/ctest.log。接口新增 key_neighbors/key_range_into，旧 IRangeLockManager 签名不变，旧算法尚未切换。key_neighbors 对保留的远端物理 ghost key 返回 present；它不表示用户可见记录存在。
