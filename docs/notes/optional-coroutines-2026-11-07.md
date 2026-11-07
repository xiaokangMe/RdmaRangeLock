# 可选协程执行（2026-11-07）

协程不是事务隔离的必要条件。它用于在等待 RDMA CQ 或事务锁时切换到同一 OS 线程内的其他事务；本地计算或单上下文路径可以直接用普通线程。

## 选项

- CLI：--execution-mode threads 或 --execution-mode coroutines。
- 单次 workload JSON 与矩阵 JSON：execution_mode 取 threads 或 coroutines。
- 矩阵环境入口：RRL_EXECUTION_MODE=threads。
- 默认 coroutines，保持已有配置行为。--coroutines 1 仍表示每个线程一个协程。
- threads 模式要求 coroutines=1；配置大于 1 时直接拒绝，不静默改变并发数。

threads 模式直接调用同一 work 函数，不构造 CoroutineScheduler、不分配协程栈、不调用 swapcontext。协作让出点在普通线程中保持 std::this_thread::yield；RDMA 完成等待由该线程轮询，不会产生协程切换。预热、事务重试、锁授权、提交/释放、延迟直方图均走相同事务骨架。本地与共享 RDMA 使用相同开关。

## 运行

    ./build-shared/rrl_workload_runner --config experiments/configs/execution_threads_smoke.json --format csv

在此命令后加 --execution-mode coroutines --coroutines 1，可比较同样活跃上下文数量下的调度差别。多协程例子为 --execution-mode coroutines --coroutines 4，此时活跃上下文数增加，不能将变化全部归因于协程调度。

CSV 新增 execution_mode，threads 的 coroutine_switches_total 必须为 0。旧的非 policy 执行路径也标为 threads。矩阵传递并记录该选项，续跑和报告按模式隔离；历史缺少该字段的 policy 行按既有 coroutines 行为解释。模式变化同时改变配置指纹，不能续写旧批次。

## 验证

- 普通线程/协程的上下文探针，预热排空、测量计数和 setup 失败退出。
- JSON/CLI 模式切换、非法组合拒绝、三种年龄策略和直方图一致性。
- 矩阵普通线程模式小型运行与续跑、报告读取、跨模式续跑拒绝。
- 全量 CTest；硬件相关测试未设置服务环境时按既有规则跳过。
- 不启动正式矩阵，以上小型运行只用于正确性验证。

验收结果：build-shared 构建成功，CTest 27 项执行通过、18 项依赖外部服务而跳过、0 失败。额外启动独立服务执行 12 个 RDMA 回环用例（next-key/record-2PL × threads/coroutines × 三种年龄策略），全部通过；线程模式切换计数为 0。验证日志位于 experiments/logs/execution-mode-check-20261107-065634/，不作为正式性能数据。
