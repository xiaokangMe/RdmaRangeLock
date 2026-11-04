# 按 Deneva 的实际调用路径对齐通信

用户明确：不是一律禁止 TCP 协调器，而是 Deneva 在对应配置、算法和阶段怎么通信，本项目就参照该路径。本文修正此前“取消 TCP”容易引起的误解；通信协议本身不是错误判据。未启动完整正式矩阵。

## 源码配置和对应关系

以 .66 的 `pgm_deneva_backup` 实际工作树为准，git 基线 a1a2f46；该目录有既有修改，不能称为干净 upstream。此次没有修改或启动 Deneva。

| 用途 | Deneva 实际路径与条件 | 本项目当前路径及差异 |
|---|---|---|
| 当前配置 | `config.h:30` 为 SIT_ALL：ONE_SIDE=true、TWO_SIDE=false、USE_COROUTINE=true；`config.h:250` 因 TWO_SIDE=false 选择 USE_RDMA=NO_CHANGE；`config.h:204` 为 TPORT_TYPE=tcp | 配置/CSV 标识 native-rdma-state，并发模型为 worker×coroutine；这不代表整个系统禁止 TCP |
| 通用消息 | `system/main.cpp:124` 初始化 tport；`transport/transport.cpp:515` 创建 nanomsg TCP 通道，`:711` send_msg；`transport/msg_thread.cpp:71` 发送消息批次 | 单服务初始化通过监听就绪与远端连接建立完成，没有复制 Deneva 的全部分布式消息框架；后续所需消息应按对应用途移植，不以 TCP 为由拒绝 |
| 初始化同步 | `system/thread.cpp:45` 发送 INIT_DONE；`system/io_thread.cpp:38` 接收并更新 setup 状态，当前配置走上述通用 TCP 通道 | 控制脚本负责本轮独立服务准备和测量边界；不是 Deneva 全部节点对等启动协议的逐行移植 |
| QP/MR 交换 | `transport/rdma.cpp:127` 的 ConnectManager 执行 wait_ready/fetch_remote_mr/cc_rc；`rlibv2/lib.hh:59` 使用 SRpc，`rlibv2/bootstrap/channel.hh:123,223,262` 使用 SOCK_DGRAM，即 UDP 控制请求 | `src/rdma/rdma_transport.cpp:537` 在 TCP 上传输 QP/MR 信息。这里只是本项目已有建链适配，不能把“Deneva 也用 TCP 建 QP”当依据；尚未统一为同一建链库/传输 |
| 索引、记录、锁和状态访问 | `system/txn.cpp` 中 read_remote_bt_node/read_remote_row/CAS/write_remote_row，以及对应 CC 的状态表访问，使用单边 RDMA；具体锁布局随算法而异 | 当前原生数据、物理锁、事务状态通过注册内存和 RDMA 访问。三类服务和独立元数据布局属于轻量适配，不声称与 Deneva 单一节点/MR 布局相同 |
| 查询来源与响应 | `SERVER_GENERATE_QUERIES=true`；`system/work_queue.cpp:245` 从本机 query 池生成请求；`system/worker_thread.cpp:346,466` 在此配置下不发 CL_RSP | 事务驱动消费本机预生成 trace；不额外增加每事务 TCP 请求/响应来替代该路径 |
| 常规多分区提交 | `system/txn.cpp:795` 仅在 is_multi_part && !rdma_one_side 时发普通 prepare/finish；`system/txn.h:273` 将当前 RDMA_OPT_NO_WAIT3 判为单边算法 | 当前原生事务在 RDMA 状态槽取得提交许可并写回。不能因为 Deneva 包含 TCP 的 2PC 代码，就把不在当前分支上的消息加入每次事务 |
| 集中逐锁授权 | 在上述选定单边路径中，锁授权由远端锁元数据/CAS 和对应算法完成，没有本项目旧式独立 wait_coordinator 的逐锁 RPC | 原生路径不启动旧 wait_coordinator；依据是选定源码路径没有这层重复授权，原因不是“TCP 不允许”。旧后端保留为显式历史对照，不能称 Deneva 同等实现 |

若后续切换 Deneva 配置或接入确实需要远端消息的业务分支，要一并移植该分支所需的 TCP/双边消息、状态机和统计，不能只根据 ONE_SIDE 宏或代码中存在 send 函数判断真实运行路径。也不能把当前轻量适配称为整个 Deneva 系统完全一致。

## 当前二进制的定向通信证据

源码版本 `f89f116f38f0a743b79e52558736c00b0213525e`。在 .66 启动独立数据/锁/事务状态服务，在 .67 对实际 runner 执行 `strace -f -yy`，记录 network、read/write/readv/writev，保存命令、配置、固定二进制哈希和原始跟踪。审计脚本随日志保存，未增加运行时依赖。

日志：`experiments/logs/communication-audit-20261104-165244-357019/`。5 个小规模 case 均通过已有 CSV 硬错误及计数校验：next-key 的 A/E，真实 leaf strict 的 A/E，真实 leaf deneva 的 E；均为 wound-wait、2 worker×2 coroutine、1 秒预热和 2 秒测量。这里只验证通信形态，不用于性能比较，也不代表另外两种策略已经完成系统调用审计。

- next-key 各有 17 个 TCP 连接；两种叶模式各有 18 个。按 TCP 四元组检查，每个连接仅一次 64 字节发送和一次 64 字节接收，均在启动建链阶段完成。
- 未发现这些 runner 在正常事务执行中另发 TCP 数据请求或逐锁授权请求；三类 RDMA 测量计数均有活动。服务列表也未启动 wait_coordinator。
- 此结论限定为本次成功路径的 .67 runner，未跟踪 Deneva 运行或所有服务进程，不替代故障分支验证；内核 TCP keepalive 不是逐事务授权。
- 此证据说明现有原生路径实际做了什么，不构成以后禁止 TCP 的验收规则。验收仍以所选 Deneva 源码分支对应关系、正确性和额外通信开销的可解释性为准。

## 本轮验证边界

当前运行时代码的完整真实 RDMA CTest 已通过 45/45，零跳过：`experiments/logs/shared-check-20261104-163743-841827/ctest.stdout`。2,097,152 key 域、32 worker×4 coroutine、range128 的短容量预检也已完成：strict 6/6，deneva 3/3，状态分别保存在 `/tmp/rrl-leaf-capacity-20261105-state.json` 与 `/tmp/rrl-deneva-capacity-20261105-state.json`。这是容量/入口预检，不是正式十秒性能数据，也不证明当前 E trace 是持续净新增负载。

固定 f89f116 二进制的三组一小时稳定性检查仍在运行：`experiments/logs/leaf-check-20261104-163836-{398726,401354,403656}/state.json`。本文没有修改运行时代码，没有重启或停止这些检查，不把未结束的运行写成已通过。

## 来源指纹

Deneva 实际源码 SHA-256：

```text
config.h                          afda7ac2729b3960fc9f8e82f6b0b711ea5b85f5d556fc8c882390aa4296ca20
transport/transport.cpp           1e8132d37e3bbea4c893fb534b1076f47999729422af93ff9ae7ecf70b133813
transport/rdma.cpp                c1fe549df3ebb0b0f1c776fc004c85a744d6253bb84a1dcc3731f0e8d4aae471
rlibv2/lib.hh                     6fcf5850835a78c01b01753f556050642fae85014939accd5792e8778c084e9d
rlibv2/bootstrap/channel.hh       312dd4576fef54867c3657891a8318db47be4dbf88fd30e7ebb852fea7d6c085
system/txn.h                     2a6b51d0021d0a3e7fafa78fa7b66a869cfa37a733aef838b75fe98f3db02701
system/txn.cpp                   6938edda8d891e37003c6726fc8ed9d8708541c838551b7bbe286e32916c2710
system/thread.cpp                5b397f16039a8437147a2d725ae3cd8ddfebf6dce6b3435a777d492eedf1e88b
system/io_thread.cpp             3a1b17d49e8592512c685f000d19594d6fd0f558774bb1b29917d127fa30055a
system/worker_thread.cpp         6405e6188fd095f0f9e19c2e99651c3e891356c91cca5526a5d1a659be112cc7
system/work_queue.cpp            3231b9ff075dffdabfba942e5ffbdb8466b4b2f67dc18fc2ed3e7ecb420eb5ca
```
