# Deneva 的元数据短锁、两百万规模与预热

## 直接源码结论

- config.h:389：SYNTH_TABLE_SIZE=2097152，是全表 key 数，不是事务数；INIT_PARALLELISM=1。benchmarks/ycsb_wl.cpp:210 的 init_table_slice 按 key 分片，先通过 table 分配 row，再 index_insert，完成后才启动负载；不是运行一条事务才生成一行。当前两节点时按 key_to_part/GET_NODE_ID 拥有分片，不能把每节点 2097152 与全局 2097152 混淆。
- storage/row.h:213..217、system/txn.cpp:2341..2563：RDMA_WAIT_DIE/WOUND_WAIT 先 CAS 记录首部的 _tid_word（0 到 owner），再 READ 持有者 lock_owner/ts，更新后清短锁。该字段作用类似元数据 gate，但每条 row 一个；其它 RDMA_NO_WAIT 分支直接在行锁字上编码 S/X，并不都是相同 gate 算法。
- 本项目的 16 个 shard gate 保护动态区间锁槽，相邻/重叠区间的原子冲突检查与发布在 gate 下完成。它比 Deneva 行级短锁粗；不能因为 Deneva 不叫 gate 就删除互斥。Deneva 这些 point-row 路径本身不提供 next-key/gap/任意谓词重叠检测。移植细粒度方案需要单独定义稳定逻辑锁对象/区间目录及 split/insert 的原子发布，不能直接把原子行锁冒充范围锁。
- config.h:651..652 的 DONE_TIMER 与 WARMUP_TIMER 都是 10*BILLION，即各十秒，行尾“~1 minutes”注释不准确。SimManager::is_warmup_done 按全局时间切换；helper.h 的 INC_STATS/SET_STATS 用该门禁；所有 worker 在预热期执行相同事务，预热不是给每条事务单独执行一次，也不保证访问全表。

## 本项目对齐与差异

保留小规模配置以复现实验，不把 10000 key 当系统能力上限。增加 deneva_scale_2m.json（单 case 数据集）和 formal_native_deneva_2m_10s.json（正式矩阵配置），key_count=2097152、10 requests/txn、10 秒并发 warm-up + 10 秒独立测量、order=128、tree_pages=131072、tree_records=4194304。按现有页布局注册内存字节数为 128+131072*(32+(2*128+1)*8)+4194304*24=374341760，约357 MiB；此外还需要客户端缓冲、锁/状态表及初始化暂存，不能将它等同 Deneva 的 1 GiB 总内存池配置。

直接复用现有可配置容量和启动入口，不把原本的 262144 record 默认容量用于两百万表。固定容量必须在装载时验证，事务期间仍由容量门禁保护，不允许部分写入。大规模 seed 构造改为按 Deneva 的装载思路直接构建排序记录集合，避免客户端/服务端为了生成同一初始集合先各插入一棵不参与测量的临时 B+ 树。业务初始集合语义与旧 seed_index 精确一致，由跨 workload 回归比较实际内容证明。

A 预装载恰为 2097152 行。当前 E 为支持插入/幻读验证，会对预生成 Insert 起始键留洞（被其它非插入请求访问的 key 仍会补入），因此 2097152 是 key 域上限，实际 seed_records 需读取服务日志。它不是 Deneva 当前默认 SCAN_PERC=0 的负载；不能把 A/E 改成只读来假称一致。Zipf 保留综述现有 .99 以便单变量比较，Deneva 当前 .4 需另设对照；本次只对齐规模、可配置请求数与预热机制。

预热方面，我们所有 worker/coroutine 同时运行真实事务、保留数据，十秒到后排空并设屏障，再独立截取测量统计/流量。Deneva 原代码按时间门禁连续切换，没有本项目这一排空屏障；保留该边界是为防止跨阶段样本混入，不能说逐行完全相同。也不声称十秒就把两百万行全部预热。

## 验证方案

先完整硬件 CTest，随后在两百万域上分别跑本地/跨主机 A/E × next-key/hierarchy 的小维度验证（并发预热保持十秒；测量十秒，单重复，明确标为非正式）。记录配置、版本、二进制摘要、实际 seed 行数/注册字节、预热提交、正式提交和错误字段；不运行完整正式矩阵，不作性能排名。检查完成后补入日志路径和边界。

## 后续 gate 优化顺序

先给出现有 gate 等待/持有与实际业务锁等待的分解数据；再为 point 记录锁提供稳定的细粒度原子锁对象，区间/next-key 保持有序目录互斥；最后扩到插入、节点 split 与范围对象的生命周期。不得用删除互斥、增加重试或延迟掩盖原子发布问题。这部分尚未实现，不混入两百万规模验证。

实际读取源文件 SHA256：

- config.h: afda7ac2729b3960fc9f8e82f6b0b711ea5b85f5d556fc8c882390aa4296ca20
- storage/row.h: d794a62a34316393b8b87c1f461438fad403785d3322067b6288ddcb68210d2e
- system/txn.cpp: 6938edda8d891e37003c6726fc8ed9d8708541c838551b7bbe286e32916c2710
- benchmarks/ycsb_wl.cpp: 79bf0c24bbe9f6da90e66f35a8a437bd118309158ca31ff767e893787155115b
- system/sim_manager.cpp: e296cfa9d5e7f32c52e92163a3386c34175affe0a385f4b57d1c6b2bd65b90c4
- system/helper.h: 2f705236f7cc819313df559a9b6cd806184b130afb969a441393b766e4501825


## 当前启用的范围锁分支补充核查

当前 config.h:234 是 RDMA_OPT_NO_WAIT3，不能把 WAIT_DIE/WOUND_WAIT 的行首短锁描述成所有 Deneva 分支。此分支使用 rdma_bt_node::intent_lock 与 row_t::_tid_word 编码四个 16 位 IS/IX/S/X 计数（从高到低；storage/index_base.h:46，解码见 system/txn.cpp:4800..4813）；index_rdma_btree.cpp:228 把实际 leaf_node_offset 返回给事务，system/txn.cpp:3815 起通过该 offset 对真实叶节点做 FAA/CAS，事务 accesses/range_node_set 保存释放位置。另有 RDMA_SINGLE_RANGE_LOCK/DOUBLE_RANGE_LOCK 分支。它没有本项目这种动态区间锁槽表，因此不需要我们的 shard gate。

必须保留的源码问题记录：system/txn.cpp:3815 的远端 RD/SCAN 分支中叶节点 IS 获取被注释，但仍记录 leaf offset；WR 获取 IX 使用 1<<32，部分失败回滚使用 (-1)<<48；rdma_opt_no_wait3.cpp:remote_write_and_unlock 留有 ignore write TODO，实际先减锁字而未复制远端业务数据。不能将这些分支当已验证的严格 2PL 实现原样搬入。上文“行路径不提供范围重叠”仅指 WAIT_DIE/WOUND_WAIT 行元数据路径，不否认该目录存在单独范围/叶节点实现。

下一次真实叶节点锁移植须通过索引的稳定叶节点身份/范围接口取得逻辑锁对象，锁模块不得直接修改页内结构；结构 latch 与事务锁保持分离。不能仅把现有逻辑 bucket 改名为 leaf。


## 两百万规模已完成的小维度验证

experiments/logs/deneva-2m-validation/logs/smoke-not-formal-20261104-150852-833227：本地 4/4、跨主机 4/4，另有两点容量预检通过；A/E × next-key/hierarchy，2 OS worker × 2 coroutine，十秒并发预热、十秒测量。每 case 有正式阶段提交，硬错误及容量/gate 中止均为零，运行后树结构验证通过。A 的服务日志 seed_records=2097152；E=2096130；注册数据区 374341760 字节。两套正式矩阵的同一路径已跑通，但该验证仅单重复、小维度，不能作正式性能比较。

本轮为验证同时运行过其它检查，不能把它当隔离后的性能基线。日志固定二进制、SHA256 和完整源码文件摘要；源树包含待提交的本里程碑改动，campaign 的旧 HEAD 字段不是这些未提交文件的唯一版本标识，复现需以源码摘要与随后提交对应。没有把测量吞吐作为论文数字发布。最大几何额外容量门禁已完成，详见文末。

最大几何容量预检已完成：experiments/logs/deneva-2m-capacity-validation/logs/smoke-not-formal-20261104-151227-047528，12/12。两百万 key 域、32 worker × 4 coroutine（128 上下文）、10 请求、range128、三策略、hierarchy bucket16/64/256；每点十秒预热+十秒测量，均有提交，容量/gate 中止与硬错误为零。只执行预检，没有启动该配置的完整正式矩阵。
