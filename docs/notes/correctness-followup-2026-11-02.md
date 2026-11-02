# 范围取值、连接生命周期、控制面期限与诊断修复方案

本里程碑处理审查清单 1–5，以 fa3d9c4 为基线。不修改 IRangeLockManager 的任何签名，不做热路径优化或正式性能矩阵。

1. v3 扫描只枚举候选 key，取得记录锁后 lookup 取值；保留其不防幻读的对照语义。v4 在扫描前取得 gap 锁，并在记录锁后重新 lookup。v5 原始扫描只用于安装 next-key 区间，校验 key 集合成功后只消费持锁扫描的 value。增加无 sleep 的确定性写入窗口测试。
2. 连接分配缓存使用进程内不重复的 generation，而非对象内从零重启的计数，并防御下标越界。用相同地址 placement-new、多连接缩为单连接、常驻线程验证。
3. TCP connect/send/recv 使用绝对期限和 poll，半包不能通过持续少量进度无限延长握手。握手被拒绝只关闭该连接，持续监听器继续服务。恢复设置整体绝对期限并用 watchdog 覆盖资源创建等同步调用，阶段期限不得重新累计。配置支持 RRL_RDMA_CONTROL_TIMEOUT_MS 和 RRL_RDMA_RECOVERY_TIMEOUT_MS，示例进 experiments/configs。保留现有单次监听兼容入口。
4. runner 在生成 trace 和初始化索引前检查参数：完整数字、有限浮点数、协议/索引/负载白名单、端口范围，以及所有 v1 多线程组合。配置文件与 CLI 同样检查，错误以 2 退出，禁止静默回退。JSON 配置解析完整的受支持输入，不用部分匹配正则提取数字；runner 接受扁平 scalar 对象，明确拒绝数组、嵌套对象及不支持的转义，矩阵描述文件仍由脚本解释。使用仓库内的轻量解析，不增加第三方依赖。
5. 每个 WR 有明确 wr_id；completion 与部分 post 失败定位实际 WR，无法定位的 timeout/poll 明确标记未知。故障快照增加进程内序号，管理器选择最新已观察故障。count 保持 size_t 签名，通过已有 Diagnostics 的错误累计数和最近 RC 区分失败与正常零。诊断扩展只追加 CSV 列。

所有新增公共内容限兼容扩展的传输层或诊断结构；范围锁接口、公共页布局和 strict 2PL 释放时机保持原契约。实现完成后在本文件补充复现命令、验证结果及未覆盖边界。

## 实现结果

- `src/txn/workload.cpp`：v3/v4 加记录锁后重新 lookup，v4 把 gap 锁移动到枚举前；v5 完成 key 集合校验后，从持锁校验扫描读取 value。v3 的幻读窗口作为对照继续保留，不能把它描述成可串行化范围锁。
- `RemoteLockManager`：连接 generation 来自进程内原子递增序号，缓存同时检查下标边界；同地址重建不会复用旧对象的连接代号。保留外部同步生命周期要求，不能在仍有线程调用旧对象时析构。
- `src/rdma/control_io.h`：poll + 非阻塞 socket 收发使用同一绝对期限，EINTR/EAGAIN 不会重新计算完整超时。客户端 connect 和 QP 属性交换共用握手预算。服务端拒绝不完整握手后继续监听。
- 服务端恢复有整体 watchdog，从建链前开始，直到恢复连接销毁后才撤销；每个 verbs 完成等待也受到相同期限约束。超时仍 fail closed，不是重试掩盖错误。`experiments/configs/rdma-correctness-timeouts.json` 记录环境变量配置示例，运行时通过 `export RRL_RDMA_CONTROL_TIMEOUT_MS=3000`、`export RRL_RDMA_RECOVERY_TIMEOUT_MS=5000` 设置；该文件不是 workload runner 配置。
- runner 在生成 trace 前拒绝未知选项/负载/索引/协议、重复选项、负数/部分数字/溢出、非有限浮点、非法端口、v1 任意协议多线程组合。支持的配置别名保留，增加 order/端口等实际读取，初始化 RC 也检查。非范围负载继续接受正式矩阵的 `range=0` 标记，YCSB-E 则要求正范围长度。扁平 JSON 只接受支持的 scalar 配置，矩阵描述文件、索引规格文件不应被当作 runner 参数文件静默加载。
- 批量 WR 的 wr_id 为一基序号，诊断 wr_index 为零基序号。错误 WC 及 partial post 使用实际 WR 地址/长度；无法定位的批量 timeout/poll 使用 `kRdmaUnknownWr`、地址/长度零。故障 sequence 是本进程内发布顺序，管理器选择当前观察到的最新快照；不能跨进程比较它。
- count 接口仍返回 size_t，通过 Diagnostics 的 `count_errors` 累计增量判断是否失败，`last_count_error` 保留最近失败 RC；成功不会清除历史计数。并发观察时错误增量可能来自其他调用，因此调用方必须保守处理，不能把它当线程局部状态。CSV 追加故障 status/tag/address/length/sequence、count 诊断和实际 order，runner 检测 count 错误也非零退出。

## 测试与复现

```sh
cmake -S . -B build-correctness-fix -DCMAKE_BUILD_TYPE=Release
cmake --build build-correctness-fix -j4
RRL_RDMA_GID_INDEX=3 python3 scripts/check_rdma_recovery.py --build build-correctness-fix
```

2026-11-02 的结果：

1. 先将 `workload_range_value_test.cpp` 单独链接到 fa3d9c4 的旧静态库，确定性复现 `driver=0 checksum=0` 的旧值提交错误。修复后 v3/v4/v5/v6 均消费正确的持锁值；插入窗口测试确认 v3 保留对照行为、v4 阻止插入、v5 检测 key 集合变化后中止。
2. .66 真实 RoCE 回环 **CTest 22/22 通过，无跳过**。包括常驻线程在同地址管理器从双连接重建为单连接后安全重新分配、真实中间 WR 的 REM_ACCESS_ERR 定位、两 QP 最新故障选择、批量超时未知 WR 标记、计数失败与正常零值区分。
3. `control_io_test` 覆盖完整交换、半包接收和发送阻塞的绝对期限。集成脚本还验证空包/半包客户端到期被拒绝后服务端仍能接收真实 QP；假服务端不返回属性时客户端按期失败；已有 owner 断开而恢复排在阻塞握手后时，整体恢复 watchdog 到期以 2 退出，日志核对 `overall recovery deadline`。
4. 既有安全恢复、StoreSlot 不确定写入与持 gate 的未知阶段崩溃拒绝测试继续通过；8 个短 workload 的 CSV、事务计数和硬错误计数通过。
5. 最终源码复制到 .67 的独立临时构建目录，未覆盖其项目工作区；.66 启动独立服务端，.67 用 `RRL_RDMA_GID_INDEX=3 RRL_TEST_RDMA_HOST=10.0.0.3 RRL_TEST_RDMA_PORT=<独立端口> ctest --test-dir <临时构建目录> --output-on-failure`，**22/22 通过，无跳过**。其中 5 个 RDMA 测试通过两台机器通信执行。另对 RDMA next-key/hierarchical 各做 100 笔事务的短负载检查，错误计数为零、事务计数守恒。这些是正确性检查，不用于性能比较。

`IRangeLockManager` 未改签名，未修改公共页布局。本次不运行正式性能矩阵或一小时压力，不声称任意时刻崩溃的透明恢复或长期无故障；fa3d9c4 的恢复边界仍有效。后续正式性能实验必须固定新提交并在两端使用一致源码，旧矩阵不能直接代表本次取值与锁顺序调整后的代码。
