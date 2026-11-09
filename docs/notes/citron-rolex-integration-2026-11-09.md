# Citron / ROLEX 统一系统接入方案

## 范围与依据

按用户授权执行 1–5：Citron 锁后端、释放/容量边界、BTreeRdma 数据路径、可选 ROLEX 索引、统一负载及 warm-up/尾延迟。只运行正确性测试与有界 smoke，不自动启动正式矩阵。

Citron 依据 FAST 2023 Gao 等 Alg.1–3 / §3.3–3.8，来源为已检查的 fuxian 独立复现。硬件使用拆字段 64-bit 原子，不宣称 masked atomic 原布局；固定树容量，无自动 scale-up/崩溃恢复。原有共享 bitmap 扩展存在多个读者释放失去保护的风险：正式接入使用论文排他范围互斥，S 保守映射为 X，输出此语义。只支持 no-wait + retry，不假装具备事务冲突身份所需的 wait-die / wound-wait。节点 ticket 在 no-wait 下 CAS 领取空闲票，避免不可取消队列阻塞。任何无法确定是否生效的远端操作失败均 fail-stop，禁止重试 FAA 清理造成双重释放。事务自重叠通过物理覆盖缓存与精确补锁处理，不能提前释放后再扩大。

ROLEX 依据 FAST 2023 Li 等及官方 iotlpf/ROLEX 提交 016a397，复用已检查的 fuxian adapter。它是本地 learned-index 适配，不宣称完整 RDMA 复现。保持 GPL-3.0 标识，独立可选构建目标；标准库替代 r2 日志/类型依赖，不新增外部运行库。只新增可选能力，不改变六版 B+ 树布局。补邻键/键范围元数据，让 gap/next/prior-key 真正走同一事务驱动；B+ 树真实叶层级不伪装成 ROLEX 叶层级。种子数据训练在测量前完成。

## 架构

索引模块仅含结构 latch；Citron 模块仅管理事务范围锁，不能读写索引页。统一 workload_runner / policy_workload 执行、种子生成、预热、事务提交、重试与延迟直方图不另写驱动。RDMA Citron 使用既有 RDMA transport 访问远端锁树。`citron_tree_height` 必须在客户端配置和服务端启动参数中一致；服务端只注册锁树内存，TCP 只交换 QP/MR 元数据，不做每次锁授权。共享数据索引和原生事务状态属于另一条 `index=rdma` 正式路径，不能与 lock-only smoke 混淆。

## 正确性门禁

验证几何覆盖、边界开闭/无界、事务自重叠、互斥和释放、失败保留/封闭、生命周期、几何不匹配、完整 64-bit 计数、单线程同 trace 结果、并发插入/扫描、ROLEX 邻键和 batch 失败原子性。接入后运行全部 CTest；RDMA 用 loopback 和双机有界 correctness smoke，独立配置及临时服务，不触碰正式服务。默认 32 线程且关闭协程用于准备的对照配置；单测用小线程验证并发。

已有 Griffin 未提交工作及历史生成物保留，提交仅包含本次里程碑。
