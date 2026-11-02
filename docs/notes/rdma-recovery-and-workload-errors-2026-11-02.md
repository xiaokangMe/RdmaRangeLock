# RDMA 故障隔离与 workload 错误传播方案

本次处理待办 1–5，不修改 IRangeLockManager 签名，不运行完整矩阵。

## 故障连接与恢复顺序

硬件 post、completion、poll 或超时失败后，先销毁本地 QP，保留诊断；再在 TCP 控制连接发送失败阶段并 shutdown。connected() 必须反映失败状态，禁止在失败 QP 上继续尝试清理 CAS。

服务端收到断连后先销毁对端 QP/MR，再使用独立的本机 RC QP 恢复。恢复与普通请求通过同一种 NIC CAS 获取 shard gate，不混用 CPU 原子与 NIC 原子。每次只恢复一个 shard，无多 shard 等待环；恢复 token 使用普通 owner 不使用的最高位。恢复只有在持有 gate 时读/压缩/写锁表，最后检查 gate 释放结果。

门闩失败、读表失败、释放门闩失败有明确阶段通知时，可以接管已死亡 owner 的 gate。StoreSlot 阶段失败可能留下非原子写入；没有通知的进程崩溃也不能推断其阶段。此时若仍持有 gate，服务端必须 fail closed 并以非零码退出，不能让其他连接读取不确定的锁表。恢复本身发生错误或超过明确的 gate 获取期限，同样终止服务端。该方案不声称实现任意时刻崩溃后的透明恢复；要实现后者仍需要日志或双缓冲发布协议。正常断连不持有 gate 时可以回收遗留逻辑锁。

## 控制连接监听

联调发现现有 accept_server 每接收一个连接就关闭并重建监听 socket。多个断连恢复连接同时建链时，尚在 backlog 的连接会被关闭，握手失败；增加重试不能解决队列生命周期错误。增加 RAII RdmaListener，由服务端在主循环外创建并保持整个进程生命周期，accept_server 复用同一个监听 fd；保留原单次监听入口兼容已有调用。该变化仅涉及传输层，不改范围锁接口。

## 返回值与统计

门闩释放检查 verbs 返回值和 CAS 前值，不把清理失败算作冲突/容量中止。workload 分别记录 lock_errors、index_errors、lock_release_failures；硬错误触发整个 run 停止派发新事务，仍让已开始事务完成 commit/abort 后清理。写入失败不计成功提交，不声称回滚已应用的写入。runner 输出错误计数后返回非零，矩阵脚本不得采纳失败运行。无锁事务的 unlock_all 返回 Abort 仅在该事务确实没有成功取得过锁时可接受。

## 范围索引与测试

load_tables 只负责读取；仅 lock 冲突检测路径构造 SlotIndex，release/count 不再排序。测试覆盖 128/129 WR 边界、畸形请求不发 WR、显式配置 RDMA 后连接失败必须失败，以及真实错误 completion 后 gate 恢复与其他 owner 锁保留。缺少 RDMA 环境变量才允许跳过硬件测试。

## 实现与验证结果

- `leave_shards()` 返回并检查 verbs/CAS 结果；取 gate 的传输失败交由已停止 QP 后的服务端恢复，不再假装在失败连接上清理成功。服务端长期持有监听器，断连清理通过独立 RC QP 的 gate 保护槽数组与 active_slots。
- `load_tables()` 不再构建 SlotIndex，只有 `lock()` 的冲突检测调用 build；`release()` 和 `get_lock_count()` 不再为读取到的槽排序。
- 五个 workload 驱动统一检查建库、写入和释放返回值。`failed_transactions` 与 committed/aborted 分开；计数关系为 transactions = committed + aborted + failed_transactions。部分写入失败归为失败事务，不声称回滚。一次硬错误停止新事务，runner 在输出诊断后返回 3。CSV 末尾追加四列，既有正式矩阵脚本已动态取表头并拒收非零退出结果；smoke 脚本也已去掉陈旧表头。
- 批量 WR 上限回归为有效 128 / 拒绝 129，检查全部结果及 1 MiB scratch 边界；批量入口在提交前拒绝零地址、空指针、溢出长度、非对齐 atomic。尾 WR 加 fence，明确前序 READ/atomic 完成边界。

复现命令：

```sh
cmake -S . -B build-recovery-fix -DCMAKE_BUILD_TYPE=Release
cmake --build build-recovery-fix -j4
RRL_RDMA_GID_INDEX=3 python3 scripts/check_rdma_recovery.py --build build-recovery-fix
```

2026-11-02 在 .66 的真实 RoCE 网卡上验证：

1. CTest **17/17 通过，0 跳过**，包括真实连接、128 WR 传输、错误 completion、失败 CAS 批次已执行前缀的 gate 回收，以及保留存活 owner 的逻辑锁。
2. `workload_error_test` 注入取锁 ERROR、释放 ERROR/AbortGate/Abort、建库失败、第一笔/部分写入后的索引失败；普通冲突/容量/gate 中止保持可继续执行。
3. 独立服务端验证 StoreSlot 阶段失败及持 gate 时未通知的进程退出，均以 2 退出，日志确认原因是“不确定写入”，不是其他建链故障。显式配置不可达 RDMA 服务端时测试返回 1，不能返回 skip 77。
4. `experiments/configs/recovery-smoke.json` 运行八个短负载检查（v1/v2 无锁、record/gap/next-key/hierarchical、RDMA next-key/hierarchical），逐项检查 100 笔事务的计数守恒、CSV 列数和硬错误计数，无硬错误。只验证可运行性，不报告性能。
5. 另做 20 次传输/锁客户端连续回归通过，`git diff --check` 与 smoke 脚本语法检查通过。

联调曾出现一次未带行号的批量校验失败，后续加入行号诊断；也复现了监听 socket 重建丢弃排队连接的问题并修复。批量完成顺序补上 fence 后，最终全套检查和连续回归未再出现校验失败；不将这次偶发失败归因于某个未经独立证实的原因，不据短测声称长期稳定性。

## 后续边界

本次没有跑完整性能矩阵、跨主机压力测试或一小时压力测试，不更新旧正式数据的性能结论。恢复只覆盖能判定安全的阶段；任意写入时刻崩溃的透明恢复仍需日志/双缓冲协议。`get_lock_count()` 的既有 size_t 接口仍不能独立表示错误，workload 清理已避免依赖它判断是否持锁。正式重测前应将相同提交同时部署到两端并使用匹配的新服务端；旧服务端不能执行新增的故障恢复协议。
