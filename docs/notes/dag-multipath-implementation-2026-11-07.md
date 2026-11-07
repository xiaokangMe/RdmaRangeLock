# DAG 多路径与共享记录保护（2026-11-07）

## 依据和实际范围

依据 Gray1976 的 DAG locking rules [@gray1976]（PDF pp.6–8）：读可以沿一条合法父路径取得祖先 IS；写必须遍历所有父路径并递归取得 IX，再取得 canonical record X。多父的共享节点不能按查询选择的某一棵树独立锁定。

本次新增两种同一机制的表示：
1. LockDag 显式资源拓扑，验证单一连通根、未知父、重复父/物理资源名、记录作为父、环和深度；共享读显式给出合法根路径，写自动计算全部祖先并按拓扑序取得意向。
2. 工作负载使用惰性生成的规则拓扑 root -> logical index root -> index bucket -> shared record。相同 RID 在所有路径只有一份权威记录 X，读路径由配置选择，写对全部索引根/桶取 IX。混合扫描/写在同一路径桶合并为 SIX + 实际 RID X。

当前多个逻辑索引投影都是 identity key，实际数据仍在同一棵权威 B+ 树（本地或服务端 RDMA）。这实现了多索引锁路径的 DAG 协议，不宣称已经实现不同 secondary key 的二级 B+ 树、原子索引维护或相应存储成本。物理叶模式和 DAG 模式是两个显式拓扑，不能混用。对隐式 X 的使用保持保守：即使所有祖先已覆盖，点写仍显式取得 canonical record X，不错误地把单条父路径的 X 当作全部父路径覆盖。

## 配置与服务校验

    --cc hierarchical --hierarchy-layout dag --bucket 16 --hierarchy-index-paths 2 --hierarchy-read-path 1

RDMA 使用 rdma-hierarchical 和 --index rdma，并接共享 data、object lock、native txn-state 服务。SharedServices 根据 DAG 布局启动 --objects，服务端根据全部 trace 键域分配规则资源目录，不构造第二份数据索引；按线程/协程配置 holder budget。DAG 与物理节点 Node 使用同一对象后端但服务 cohort 独立，不能混用同一资源目录。

ObjectLockManager 新增 configure_dag / matches_dag 的配置能力，旧 IRangeLockManager 签名不变。对象 header 的保留字段用于精确公布 dag_width/dag_paths，magic=RRLOBJ03；客户端和事务驱动都校验宽度与路径数。拒绝旧客户端、不同桶宽/路径数的客户端，避免相同逻辑记录被不同资源ID保护。read_path 可不同，不改变共享拓扑。range region仍5，TCP仍RRLWAIT4。

路径数范围2..32、read_path必须有效，节点ID/容量检查溢出。桶宽须显式数字，拒绝 auto（客户端/服务端自动策略不一致会产生错误锁名）。DAG仅支持local/native RDMA，不接受TCP镜像协调或Deneva早释例外。本次桶升降仅固定桶模式，DAG不启用粒度升降，避免把单路径祖先覆盖误当整个DAG覆盖。

## 验证

构建成功，真实 RDMA 回环完整 CTest52/52，通过且无跳过。
日志：experiments/logs/classic-mgl-check-20261107-085341/ctest.log。
- 显式菱形拓扑：不同路径上的覆盖 S 排斥全部父路径写；反向到达同样排斥。
- 读一条合法路径，共享 canonical RID；不同记录保持可并行；生成规则拓扑保护空扫描插入。
- 本地 A/E × 三种等待策略 × 2/3条路径 × 各读路径：与统一事务骨架参考结果的 checksum/行数/提交数一致。
- 真实 RDMA A/E × 三种策略 × 双线程/双协程，服务端数据访问；错误拓扑/旧客户端接入拒绝。
- 正式矩阵脚本将DAG参数及粒度转换参数传入实际CLI，并校验CSV配置一致、续跑一致；没有运行正式矩阵。

## 复现配置

experiments/configs 下新增：
- classic_key_correctness.json：prior-key，切换 --cc gap-lock/next-key 可复用。
- mgl_bucket_conversion_correctness.json：固定桶运行时升降。
- mgl_dag_correctness.json：双路径DAG，第二路径读。

本地：
    build-shared/rrl_workload_runner --config experiments/configs/mgl_dag_correctness.json --format csv

上述三配置已逐个执行，均有提交且无硬错误。它们是小规模正确性配置，不是正式性能数据；正式实验需要另定容量和规模。旧实验数据保持原commit归属。
