# Wait-Die / Wound-Wait、延迟分布与谓词锁实现记录

## 交付范围

三种冲突策略已进入可运行代码，通过 `--wait-policy no-wait|wait-die|wound-wait` 切换。本地和服务端协同 RDMA 使用同一事务驱动、同一年龄规则。原 `IRangeLockManager` 四个方法签名不变，索引布局和 latch 实现不变。默认不带新选项的旧命令继续走旧驱动；新策略实验必须三种策略都显式传 `--wait-policy`，不能把旧路径 NO_WAIT 与新路径等待策略当成单变量对照。

本里程碑还包含固定内存延迟直方图、逻辑事务重试、完整区间 Predicate Lock 驱动。不引入 B-link、乐观下降、新负载，不重跑正式矩阵。

## 代码入口与事务顺序

| 文件 | 职责 |
|---|---|
| `include/lock/wait_policy.h`、`src/lock/wait_policy.cpp` | 独立生命周期接口、年龄判定、CV 等待、wound、提交仲裁 |
| `src/lock/wait_control.cpp`、`benchmarks/wait_coordinator.cpp` | TCP 控制面服务与每 worker 一个客户端连接 |
| `src/txn/policy_workload.cpp` | 所有协议共用的事务执行、重试、延迟与释放顺序 |
| `include/common/latency_histogram.h` | 固定 3776 桶统计；计时结束后导出完整桶 |
| `benchmarks/workload_runner.cpp` | JSON/CLI 配置、新 CSV schema、旧模式兼容 |
| `scripts/check_wait_policies.py` | 全 CTest、30 个本地及 18 个跨机定向 smoke 校验 |

一次 attempt 顺序为：Begin 分配身份和年龄 → 每次请求先获得协调授权，再安装物理锁 → 读及暂存写集合 → PrepareCommit → 应用写集合 → 释放物理锁 → End 删除协调元数据、通知 waiter。冲突、中止、wound 或超时先决定 abort，再统一释放；中止之前不应用写集合。

PrepareCommit 与 wound 在同一协调器 mutex 下竞争。提交许可先成功的事务进入 committing，之后不能申请新锁，也不能被 wound；wound 先发生则提交许可失败。被 wound 的事务不会被远程线程强行删锁，而是在操作边界、阻塞请求返回和提交前识别撤销并自行清理。其等待其他资源时也会被 CV 唤醒。这样不会发生物理锁尚未清理，元数据却已允许冲突者进入的问题。

写应用发生硬错误时，整个 run 失败，不把局部写入伪装成成功回滚；本项目没有 WAL、通用 undo 或分布式数据库提交协议。RDMA 实验仍研究共享远程锁表，索引数据是 benchmark 进程内的 B+ 树，不代表跨进程共享数据存储。

## 三种策略及正确性边界

年龄值越小越老，由同一个协调器单调分配，不比较 `.66/.67` 的物理时钟。每次重试获得新的 attempt ID，继承该 session 上同一逻辑事务的年龄；新逻辑事务重新分配年龄。禁止其他 session 冒用身份或继承年龄。

| 策略 | 请求者较老 | 请求者较年轻 |
|---|---|---|
| no-wait | 冲突立即中止自身 | 冲突立即中止自身 |
| wait-die | 所有冲突持有者都较年轻时等待 | 有更老持有者则中止自身 |
| wound-wait | 标记较年轻持有者 wounded，等待其完成释放 | 等待较老持有者 |

每次唤醒都重查全部冲突者；同资源升级先计算最终合并模式，再检查冲突，不能用自持 S/IS 跳过升级冲突。Wait-Die 的等待边沿年龄递增；Wound-Wait 的正常等待边沿年龄递减，wounded 事务只清理退出，committing 事务不再请求新锁，因此不会形成继续申请锁的等待环。

一个 active cohort 禁止混用三种策略；全部事务 End 后可以切换。CV 使用事务 deadline，禁止产品端 sleep、轮询锁表或随机退避。`max_attempts` 和测量窗口有界，超时/重试耗尽单列；这不承诺每个事务都完成，也不承诺操作系统级公平调度。用户授权的事务重试不是用于掩盖死锁的重试。

## RDMA 部署限制与故障处理

这是 **TCP control + RDMA slots** 模式：TCP 负责身份、等待、撤销、提交许可；原 one-sided RDMA 路径负责物理槽表的发布、互斥和释放。每次授权都经过协调器，当前协调元数据使用全局 mutex 和线性冲突检查，是正确性基线，不能据此声称等待策略已经扩展到无中心高并发实现。

每轮使用独立物理锁服务器和单一协调器。新客户端在 connect 前启用协调器全局 attempt ID，避免不同客户端的 namespace 映射混淆事务。**禁止在同一物理表混入旧客户端或第二个协调器**；当前部署边界由运行脚本保证，服务端没有自动注册绑定检测。三种策略比较必须使用相同控制面、连接数和物理容量。

活跃 holder 控制连接断开时，协调器保留不确定的锁元数据并 poison：唤醒 waiter 返回 ERROR，拒绝新 Begin/授权/提交。不得通过删除元数据或 lease 到期直接放行，因为旧 NIC/物理锁可能尚未结束。故障 run 作废，确认全部客户端退出后重启该轮独立协调器和物理服务器。没有实现服务在线恢复、持久化日志或高可用。

阻塞请求受事务预算约束，socket I/O 有超时；连接正在服务一个阻塞请求时，断线的识别可能延后到该请求的 deadline/应答，不能把 socket idle timeout 当成异步撤销的精确上限。服务端退出或协议 I/O 错误使客户端关闭失步连接并结束失败 run。

RDMA 协调预算由服务端 `rrl_wait_coordinator` 的 `lock-budget` 启动参数决定，客户端 `--lock-budget` 不会远程修改它；本地则直接使用该配置。`lock_budget` 是每个 attempt 预留的协调锁条目上限，超过返回硬错误，不在持锁时扩容。这与 RDMA 物理锁槽容量是两项限制；本次 4 worker 小 trace 验证不能替代后续正式矩阵的容量确认。

## 指标口径

新 CSV 追加 `metrics_schema=2`、`wait_policy`、`coordination`，原列及默认旧 CSV 不变。

| 指标 | 分母/含义 |
|---|---|
| committed throughput | 最终提交的逻辑事务数 / 本轮实测时间 |
| successful_attempt | 仅最终成功 attempt：Begin 前到物理释放及 End 后 |
| logical_completion | 仅最终完成的逻辑事务：第一次 attempt 前到最终 End 后，包含失败 attempt 和重试 |
| logical_unfinished / logical_failed | 窗口截止/重试耗尽未完成，与硬错误失败分别列出；不填零延迟 |
| retries / completed_retries / unfinished_retries | 总额外 attempts，与最终完成/未完成逻辑事务的重试分别计数 |
| max_retries / retry_exhausted | 每逻辑事务最大重试次数，以及触达 attempt 上限的数量 |
| wound_aborts / deadline_aborts | 因撤销和事务预算耗尽中止的 attempt 数量 |
| lock_wait | 协调器冲突循环发生阻塞后，到获准/中止的驻留时间；含服务端 CV 唤醒及重新竞争 mutex；不含网络往返 |
| lock_call | 控制请求加物理锁申请的调用时间；包含 lock_wait，不能与其简单相加 |
| release | 物理释放加 End 控制请求的时间 |

五类延迟均报告 count、total_ns、max、p50/p95/p99/p999，完整桶通过 `--latency-histogram PATH` 导出。固定桶对小延迟精确，较大值分位数上界的相对误差小于 1/64。直方图在所有 worker 退出后、计时窗外写盘；初始化、预生成 trace 和层级计划不在窗口内。结束后的 B+ 树结构校验也在计时外。

计数必须满足：

```
attempts = committed + aborted + failed_transactions
logical_started = committed + logical_unfinished + logical_failed
attempts = logical_started + retries
retries = completed_retries + unfinished_retries
successful_attempt_count = logical_completion_count = committed
```

NO_WAIT 的 lock_wait 为 0；等待为 0 不代表延迟低，必须同时看 logical_completion、完成比例和重试。只统计最终完成事务的尾延迟存在完成者选择偏差，因此必须同时展示 unfinished 比例，不能单看 p99 宣称更优。完成事务平均重试用 `completed_retries / committed`；不要使用含未完成重试的总量冒充这个指标。p999 必须带样本量，样本不足时标为不稳定，不混合独立 run 的 histogram 计算一条分位线。RDMA QP/gate/I/O 原诊断列继续保留，与事务等待分别解释。

## Predicate Lock

`--cc predicate` / `--cc rdma-predicate`：范围读在扫描前锁完整 `[l,r)` S；点读 S，写/插入/删除点 X，均使用 Predicate 类型。空结果也保护，中间插入被阻止，右开边界可写。范围授权先于扫描，因此不需要使用扫描后候选集合校验来补幻读窗口。

这是单键一维区间谓词锁，不是任意 SQL 谓词、SSI 或 B-link。旧 record/gap/next-key/hierarchy 驱动保持各自锁对象的差别，Record 仍不提供区间幻读保护。

## 使用方法

在 `.66` 仓库构建：

```sh
cmake -S . -B build-wait-policy -DCMAKE_BUILD_TYPE=Release
cmake --build build-wait-policy -j4
build-wait-policy/rrl_workload_runner --config experiments/configs/wait_policy_smoke.json --wait-policy wait-die --latency-histogram /tmp/local-wait-die-hist.json --format csv
```

分别使用 `no-wait`、`wait-die`、`wound-wait`，其余选项一致。`--cc` 可选 `record-2pl`、`gap-lock`、`next-key`、`hierarchical`、`predicate`。重试上限 `--max-attempts`，attempt 总预算 `--wait-timeout-ms`（1..600000 ms），协调锁预算 `--lock-budget`（1..1048576）。

RDMA 在 `.66` 两个终端启动本轮专用服务：

```sh
RRL_RDMA_GID_INDEX=3 build-wait-policy/rrl_rdma_lock_server 19896
build-wait-policy/rrl_wait_coordinator 19897 16384 10000
```

确保端口未占用，`.67` 使用从本次构建复制、SHA256 相同的 runner 与同一配置：

```sh
RRL_RDMA_GID_INDEX=3 ./rrl_workload_runner --config rdma_wait_policy_smoke.json --wait-policy wound-wait --latency-histogram /tmp/rdma-wound-wait-hist.json --format csv
```

配置里的 `10.0.0.3` 是本环境 `.66` 的 RoCE 地址，管理 SSH 仍使用 `10.102.35.66`。RDMA 可选 `rdma-next-key`、`rdma-hierarchical`、`rdma-predicate`；显式策略模式缺协调器端口会立即拒绝，不退回 no-wait。协调器第三个参数是连接 idle timeout，实际配置应大于 worker 的最长空闲期。

一条命令执行有独立端口和二进制校验的正确性检查：

```sh
python3 -u scripts/check_wait_policies.py --cross-host
```

只停止该脚本创建的服务；不控制旧矩阵，不写旧结果。跨机部署用 `.66` 已配置的部署 key，在 `.67` 的临时独立目录执行，不修改 `.67` 原工程。

## 验证记录与验收边界

2026-11-03 最终源码验证位置见同目录后续记录及 `experiments/logs/wait-policy-*/validated.json`，日志和原始 CSV 按约束不入库。已验证：

- 完整 CTest 31/31 通过，无硬件用例跳过。
- 30 个本地有限 trace（3 策略 × 5 协议 × 2 负载），18 个 `.67 → .66` 一秒定向检查（3 × 3 × 2），均通过两层计数守恒、延迟样本数及硬错误检查。
- 独立控制连接、独立 QP 验证较年轻中止/等待、较老等待/wound、物理释放先于授权唤醒，以及最终槽表归零。
- 两锁相反申请次序、S→X 升级、提交许可与 wound 竞争、wounded waiter 取消、超时、连接断开 poison、服务退出、空结果 Predicate 和边界检查。
- JSON/CLI 非法配置、CSV schema、完整 histogram 与分位数、B+ 树结束后结构检查。

这完成策略实现与定向正确性接入，**不等于新正式性能基线或整模块实验验收**。已完成下述一小时定向压力验收，未运行新正式矩阵；不能用短跑给出吞吐排名、中止率一定下降、无饥饿或所有故障恢复正确的结论。后续更大规模的物理槽容量确认、同路径三策略正式矩阵和论文图表更新应单列执行。

## 文献核对

源头书目信息为 Rosenkrantz、Stearns、Lewis，TODS 1978，3(2):178–198，DOI `10.1145/320251.320260`。原文 PDF 当前访问返回 403，不能声称已下载或精读原文。

已下载 Bernstein / Goodman 的 1981 综述并重点阅读 3.1–3.5（印刷页 194–198），核对 2PL、中央锁调度、等待图、重试身份与提交阶段不可随意撤销的区别。注意其印刷页 197 的策略方向表述与现代通行命名及同页叙述有不一致，不能直接照搬条件。实现按 CMU 15-445/645 Spring 2025 官方 Two-Phase Locking 课件第 36–38 张核对：Wait-Die 老等年轻死；Wound-Wait 老撤年轻、年轻等；重试保留原年龄。PDF 实际页 85、88–93 含课件动画重复页，已读取相关内容。

本机下载文件：`papers/bernstein-goodman-1981-concurrency-distributed-databases.pdf`、`papers/cmu-2025-two-phase-locking.pdf`。论文只作为规则依据；TCP 控制协议、全局 attempt ID、commit fence 和故障 poison 是本项目在既有接口上的工程设计，不声称原论文实现了这些 RDMA 机制。

参考来源：

- https://doi.org/10.1145/320251.320260
- https://pages.cs.wisc.edu/~yxy/cs839-s20/papers/concurrency-distributed-databases_1981.pdf
- https://15445.courses.cs.cmu.edu/spring2025/slides/17-twophaselocking.pdf


## 额外同步检查：GCC 11 ThreadSanitizer

`.66` 的 GCC 11 + glibc 2.35 下，默认 sanitizer 检查对 `steady_clock` 的 `pthread_cond_clockwait` 路径报 double-lock/data-race。不能仅凭告警假定实现错误，也不能直接忽略：用完全独立、仅含 mutex/CV/两线程的最小程序复现了相同告警；其数据都在同一个 mutex 下读写。

在 sanitizer 专用编译中，预包含一个临时头，先包含 `<bits/c++config.h>` 再取消定义 `_GLIBCXX_USE_PTHREAD_COND_CLOCKWAIT`，使 libstdc++ 选择检查器能够跟踪的备用计时等待路径。没有加入 suppression，没有放宽测试，也没有更改生产代码。该专用构建的 `rrl_wait_policy_test` 与 `rrl_wait_control_failure_test` 2/2 通过，无告警。生产 Release 仍使用 steady-clock 计时路径；一小时及真实 RDMA 验证使用生产二进制。这是有工具链兼容条件的 sanitizer 检查，不宣称默认 GCC 11 TSan 直接通过。

最小复现源码、兼容头、原始告警与专用构建通过日志保存在本轮 endurance 的 ignored 日志目录。将来升级检查器后应重新验证原生拦截路径，不把临时编译兼容选项加入默认 CMake。


## 构建与内存检查

已在独立临时源码目录完成全目标 Release 构建，未复用原构建目录。ASan+UBSan 专用 Debug 构建的策略、真实 TCP 断线和运行器/完整直方图测试 3/3 通过。上述检查不改变生产编译选项，不替代真实 RDMA 用例或一小时生产构建压力验证。构建与 sanitizer 日志保存在同一轮 ignored 日志目录。


## 最终长时验收（2026-11-03）

使用 `experiments/configs/wait_policy_endurance.json` 与 `python3 -u scripts/check_wait_policy_endurance.py --seconds 3600`，在同一生产构建上完成 6 个任务，各连续运行至少 3600 秒，均 PASS：

| 路径 | 协议 | 策略 | 持续时间 | 结果 |
|---|---|---|---:|---|
| 跨机 RDMA | next-key | wait-die | 3600.001 s | PASS |
| 跨机 RDMA | next-key | wound-wait | 3600.001 s | PASS |
| 本地 | hierarchical | wait-die | 3600.000 s | PASS |
| 本地 | hierarchical | wound-wait | 3600.000 s | PASS |
| 本地 | predicate | wait-die | 3600.001 s | PASS |
| 本地 | predicate | wound-wait | 3600.000 s | PASS |

全部结束后通过两层事务计数守恒、延迟桶样本数量、硬错误与 B+ 树结构校验；跨机每种策略结束后又以独立 session/QP 执行合同用例并确认物理锁表归零。所有服务仅在本脚本的任务结束后关闭。6 个任务并行运行，因此这些数据是持续正确性证据，不能用于策略性能排序。

最终短回归：`experiments/logs/wait-policy-20261103-125816/`（CTest 31/31，无跳过；30 本地 +18 跨机 smoke；另含独立跨机合同用例）。一小时证据：`experiments/logs/wait-endurance-20261103-125912/`（`state.json`、每 case 命令、stdout/stderr、完整 histogram、跨机 post-contract 日志及 sanitizer/干净构建记录）。原始数据均保持 ignored，不放入提交。

测试时 HEAD 为 `e26fad8` 加本里程碑工作区改动，不能把该旧 HEAD 单独当作测试源码版本。两个证据目录均保存 `source-files.sha256.json`；最终提交后再写 `committed-source-provenance.json`，逐文件核对已提交源码与被测源码一致，并记录正式提交和二进制 SHA256。先前未满一小时的 `wait-endurance-20261103-125327` 已标记 SUPERSEDED，不计入验收。

生产 runner SHA256：`4c46e4c4394a654fe900a8d62354cd842fffd82b26022a8a603f7eb4e972a531`。两台机器使用相同 runner 二进制哈希。一小时定向验证完成代表本次等待策略可运行与持续正确性检查通过，不替代正式矩阵、部署故障恢复或所有负载上的完整数据库正确性证明。
