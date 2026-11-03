# Deneva 数据路径迁移与验收（2026-11-04）

本次交付把权威索引和记录接入共享 RDMA 内存，修正此前只远端存锁、客户端各有本地数据的架构缺口。旧数据不删除，归类为锁服务微基准；新共享数据正式矩阵尚未运行。

## 实现

以 .66 的 pgm_deneva_backup 为参考，核对 transport/rdma.cpp 的注册内存、storage/index_rdma_btree.cpp 的根/子/叶链偏移、storage/table.cpp 的记录区，以及 system/txn.cpp 的 read_remote_btree_index/read_remote_bt_node/read_remote_row/write_remote_row。保留这些数据路径概念，适配项目的 uint64 key/value 和公共索引/锁接口；未搬入 Deneva 的工作线程框架、协程、TPC-C 专用强转、退避和已发现的不安全释放访问。未增加第三方依赖。

- 新 rrl_rdma_data_server 初始化一次注册内存；所有客户端访问同一权威树和记录，不再按客户端重新灌数。
- 公共 offset 线格式与主机页布局在 include/index/btree.h；所有 RDMA 协议共用 BTreeRdma，避免协议间改变数据路径。
- 按 key 比较逐层读取远端内部/叶节点，再读取远端记录；批量写前预检容量，在共享/排他结构 latch 下应用；页分裂维护双向叶链，删除为 tombstone。
- 原 IRangeLockManager 方法签名不变；事务只通过稳定的索引接口访问数据，不触碰页内结构。所有读写缓冲在 Begin 前准备。
- 读自己之前的写入在统一驱动中合并，包括点查和范围查；PrepareCommit 先于写入；所有写完成后才统一释放物理锁和 End。
- 数据故障置 poison，不确定写入不释放事务后继续测量。结构 latch 的 CAS 采用绝对超时，超时为硬错误；没有 sleep/随机退避。无持久化、崩溃恢复或在线重启承诺。
- CLI 对 rdma 协议要求共享索引及数据服务。旧本地索引/远端锁表需显式 --rdma-lock-only 1，输出 data_backend=local。

## 验证范围

验收分为全 CTest、三策略五协议四负载的小窗口检查、双机独立进程共享可见性/幻读交错/精确累加、两套正式入口的缩小验证、正式几何边界定向检查与独立一小时压力。原始日志在 experiments/logs，不入 Git；各项检查对应的源码指纹单独保留。

所有压力和 smoke 仅用于正确性与运行兼容验收，不能用并发执行时的吞吐排名；正式矩阵仍需用户决定后另行运行。

## 明确限制与后续

1. 结构写批次仍由全局排他 latch 串行；当前主要目标是正确共享数据，后续才能做页粒度 latch/缓存等扩展。
2. 数据固定容量、删除不回收，写前容量检查保守；容量或 gate 中止视为正式 case 无效。
3. 事务协调面仍为 TCP，物理锁和数据为单边 RDMA；不能称整个事务完全 one-sided。
4. 本地页是含 C++ 容器的主机格式，远端页是偏移格式；只在每组内部比较锁协议，不能把本地/RDMA 绝对差异都归因于锁。
5. no-wait 高冲突可能没有提交；该结果需保留无进展标记和未完成计数，零样本不作尾延迟结论。

收尾复核修复了一个测量门禁缺口：worker 写入容量不足计数未汇入 RunStats，可能让正式脚本漏判。现在汇总 capacity/gate 计数；两组矩阵统一拒绝非零计数。新增三策略下确定性拒绝写批次的回归检查，验证中止可见、事务/锁正常清理；并重跑最终二进制的完整边界定向检查。


## 已完成的验收记录

原始目录名采用 .66 的 UTC 时钟，本文日期采用用户 Asia/Shanghai 时区。

| 检查 | 结果 | .66 上的证据 |
|---|---|---|
| Release 全 CTest | 35/35，无跳过 | experiments/logs/shared-check-20261103-164635-406034/ctest.stdout |
| 三策略 × 五协议 × 四负载 | 60 个跨主机小窗口 case 通过 | 同目录 validated.json、progress.json 与每 case stdout |
| 双机真实共享 | .66/.67 各 500 次受保护累加，最终增量精确为 1000；另一进程确认提交值 | 同目录 concurrent-increment-*.stdout、cross-increment-exact-sum.stdout |
| 幻读与故障交错 | 独立进程插入受 range 锁拦截；运行中数据 poison 导致 runner 错误退出，coordinator 拒绝新事务 | 同目录 cross-process-phantom.stdout、active-failure.stdout/stderr |
| 正式入口缩小验证 | 最终版本地 40、跨主机 16；含固定二进制和配套服务路径 | /tmp/rrl-shared-final-updated-entry.log，及对应 smoke-not-formal campaign |
| 正式几何边界 | 10000 key、5000 trace、32 线程、10 ops、YCSB-E range 128；两协议 × 三策略 × 四负载：本地 24、跨主机 24，全通过 | experiments/logs/smoke-not-formal-20261103-171133-504049/final-audit.json；最终计数修正版重跑，48 case 均有提交，容量/gate 中止均为零 |
| 干净导出构建 | 独立源码目录构建成功，35/35 与 60 项跨主机检查通过 | /tmp/rrl-shared-final-clean-8axg8ytd；/tmp/rrl-shared-final-clean-check.log |
| ASan + UBSan | 树分裂/范围/故障传播、own-write、策略测试 3/3 | /tmp/rrl-shared-asan-check.log |
| TSan | 条件兼容构建的策略与 own-write 测试 2/2，无告警 | /tmp/rrl-shared-tsan-check.log；仅 sanitizer 使用既有 clockwait 兼容头，详见 wait-policies-and-latency-2026-11-03.md；不宣称原生 GCC 11 TSan 无条件通过 |
| 最终容量回归 sanitizer | 新增三策略容量拒绝用例，ASan/UBSan、条件兼容 TSan 各 1/1 | /tmp/rrl-shared-final-aggregation-asan.log、/tmp/rrl-shared-final-aggregation-tsan.log |
| 一小时压力 | 六组均完整 3600 秒以上，结构/计数/数据读写检查通过 | experiments/logs/shared-endurance-20261103-162241-872237/state.json、final-audit.json |
| 结果与绘图门禁 | 默认拒绝 smoke；显式 --allow-smoke 后生成汇总/图表并标注非正式结果；已检查图像 | boundary 目录 report-validation/report.md 与 PNG |

完整正式矩阵未启动。小窗口、边界检查和并发压力的吞吐不能用于协议排名。默认正式维度的 756/432 case 枚举由 rrl_shared_matrix_gate_test 检查；新正式任务还会先运行完整正确性检查与容量预检。

## 一小时压力与源码一致性

六组独立服务并发执行，互不共享锁表/数据，不是性能排名：跨主机 next-key/hierarchy × wait-die/wound-wait 四组，以及本地 RDMA 回环 hierarchy × 两策略两组，六组均 PASS，每组实际测量 3600.0003 秒以上，计数守恒、结构验证和硬错误检查通过，数据读写均非零，本机自有服务全部退出。逐组结果与 CSV 审计在 final-audit.json；验收日志位于 experiments/logs/shared-endurance-20261103-162241-872237。

测试 runner SHA256 为 `d49e65b3267eccb29ab4579ec9e001808d04a14c941025d704466609abd38035`；四个 .67 独立 runner 文件已核对同一哈希。压力启动后的最终生产差异仅为 src/txn/policy_workload.cpp 汇总阶段补加 capacity/gate 两项计数；其余 37/38 个生产 C++ 源/头文件字节不变，结构算法、锁、事务执行阶段不变。压力使用的是该计数修复前的 runner，不冒称最后二进制逐字节通过一小时；最终二进制另行通过完整 CTest、60 项跨主机检查和正式几何边界定向检查。两份源码指纹与汇总差异均保留在压力目录的 committed-source-provenance.json。测试中两个早期短轮次因统一驱动语义和 CLI 门禁完善而标记 SUPERSEDED，不计入一小时通过证据。


一小时 runner 的容量汇总有上述已修复缺口，不能把其输出零容量中止当作最终容量证明。容量门禁证据以最终二进制的 48 个十秒边界 case 和提交后的正式路径预检为准。
