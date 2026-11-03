# RDMA 层级锁：保护粒度与事务进展修复方案

基线：230682f；保留 NO_WAIT，不引入 Wait-Die，不重跑正式矩阵。

## 已确认的问题

正式批次 verified-20261102-225419 的 YCSB-A / bucket=256 / threads=4 三次十秒运行只提交 1、2、0 个事务。仍有十余万次尝试，传输、容量、gate 和释放错误均为零。不能将其归为 NIC 超时或已证明的死锁。

修复前驱动对点访问取得整桶 S/X，并逐操作升级。热点下不同记录相互排斥，多个读者后续升级容易相互中止。QP 与本地 gate 的 std::mutex 不承诺 FIFO，无法从静态代码保证已排队的释放不被后来的请求反复超越。后者是进展风险，不声称已实测为唯一根因。

## 修复设计

1. 事务访问集已知，运行前生成同一份层级锁计划。本地与 RDMA 共用驱动：根 IS/IX；点访问使用桶 IS/IX 与记录 S/X；范围访问使用覆盖桶 S。按根、桶 ID、记录 key 排序申请，合并同资源需求，以最终模式一次申请，执行中不升级。
2. 同一桶同时有范围读和写时一次申请桶 X，保守代替尚未提供的 SIX，不新增锁模式或修改公共锁接口。桶 S/X 已覆盖的记录不重复申请。空范围无数据访问不加桶锁；非空但无结果的范围照常加桶 S，防止插入幻读。
3. 在共享 RangeLock 冲突判断中区分桶意向与记录数据锁。Node IS/IX 不与 Record S/X 直接竞争；同桶 Node S 对 IX 以及同记录 S/X 仍冲突。根意向原有规则保留。桶边界以统一 key 比较表示，width=1 和 UINT64_MAX 需测试。
4. 共享 QP、事务操作条带与本地 gate 使用条件变量 FIFO 准入。无 sleep、轮询重试或随机退避，不更改事务锁 NO_WAIT 语义。保持既定资源顺序：事务条带 -> QP -> 升序 gate -> 远程 CAS。每次远端操作完成后让出准入权，释放与申请进入同一 FIFO。
5. 可选诊断输出到独立 JSONL（stderr），默认关闭，不改正式 CSV：每 worker 提交/中止、各层冲突、最后冲突资源、最长无提交时间、最长中止串、申请/收尾时长。运行后统一输出，不在持锁期间落盘。

## 验证边界

先增加可复现的本地保护语义、模式合并、FIFO 与清理回归，再跑全量 CTest 和真实 QP 定向检查。检查同桶不同记录并发、同记录互斥、范围与插入冲突、跨桶与空结果边界、部分申请失败清理、传输错误保持硬失败、共享少量 QP 的各线程进展和最终远程锁表归零。仅执行有限的定向运行验证，不形成新正式矩阵或性能排名。

旧矩阵保留为旧实现数据。修改访问粒度与准入后必须重新测量才能更新性能结论，当前不重跑正式矩阵。


## 完成项与验证结果

上述五项设计已落地；同时修复了模式转换的冲突检查：四模式 API 将 S + IX 保守合并成 X，申请时必须按最终 X 检查其他事务，而不能只检查新传入的 IX。本地和远端使用相同规则，远端复用 owned-slot 查找结果，避免重复查找。

最终验证记录位于 `experiments/logs/hierarchy-final-20261103-085634/`（原始日志不入库）。构建为 Release，验证代码基于 `230682f` 加本轮补丁，测试发生在提交前；提交后以该目录中的 `source-provenance.json` 关联提交和文件哈希，不能把测试时 HEAD 误写成补丁提交。

- 真实 RDMA 环境下 CTest **27/27 全部通过，无跳过**。包括已有异常注入、恢复、释放、诊断和工作区边界测试，以及新增的层级保护、计划与 FIFO 测试。
- 跨机方向为 `.67 client -> .66 server`，RoCE 地址 `10.0.0.3`、独立端口 `19893`、GID index `3`。8 线程共享 2 个 QP，16 轮屏障同步：每轮允许同桶不同记录同时持有 X，每个线程全部完成，最后远端锁表为 0。
- 对原退化形态做了 **2 秒定向进展检查**：YCSB-A、4 线程、桶宽 256、Zipf 0.99、每事务 10 操作、2 个 QP。真实 QP 回环和跨机均通过 `scripts/check_hierarchy_progress.py`：每线程有提交、计数守恒、根/桶冲突为 0、无运行时升级，容量、gate、传输、计数和释放等硬错误为 0。这是功能进展检查，不用于吞吐排名或加速比。
- 跨机部署使用独立临时目录，逐文件校验测试程序、runner 和配置的 SHA256；不改另一台机器原项目。验证结束后只停止本次启动的服务端进程，无残留验证进程。

语义测试覆盖同桶不同记录并发、同记录互斥、范围与插入冲突、反向到达、半开上界、空区间与空结果保护、width=1、最大 key 防溢出、计划排序与去重、最终模式冲突、部分申请失败清理。新增诊断默认关闭，CSV 格式保持原样。

## 手动复现定向验证

在仓库根目录构建：

```bash
cmake -S . -B build-hierarchy-fix -DCMAKE_BUILD_TYPE=Release
cmake --build build-hierarchy-fix -j4
```

终端 A 在确认端口未被占用后启动专用服务端，看到 listening 再执行终端 B：

```bash
RRL_RDMA_GID_INDEX=3 build-hierarchy-fix/rrl_rdma_lock_server 19893
```

终端 B 运行正确性测试与配置中的短时定向检查：

```bash
export RRL_RDMA_GID_INDEX=3
export RRL_TEST_RDMA_HOST=127.0.0.1
export RRL_TEST_RDMA_PORT=19893
ctest --test-dir build-hierarchy-fix --output-on-failure
mkdir -p experiments/logs/hierarchy-directed-manual
RRL_RDMA_PROFILE=1 build-hierarchy-fix/rrl_workload_runner \
  --config experiments/configs/rdma_hierarchy_diagnostic.json --format csv \
  > experiments/logs/hierarchy-directed-manual/loopback.csv \
  2> experiments/logs/hierarchy-directed-manual/loopback-progress.jsonl
python3 scripts/check_hierarchy_progress.py \
  experiments/logs/hierarchy-directed-manual/loopback.csv \
  experiments/logs/hierarchy-directed-manual/loopback-progress.jsonl
```

跨机时，将同一构建的 `rrl_rdma_hierarchy_test`、`rrl_workload_runner` 和同一配置复制到 `.67` 的独立目录，校验哈希后运行；设置 `RRL_TEST_RDMA_HOST=10.0.0.3` 执行语义测试，runner 以 `--rdma-host 10.0.0.3` 覆盖配置。使用同一 checker 验证 CSV 与 JSONL。完成后在终端 A 用 Ctrl+C 停止自己启动的服务端，不使用按进程名批量停止。

诊断 last_conflict 中 phase 0/1/2 表示根/桶/记录，mode 0/1/2/3 表示 IS/IX/S/X；max_no_commit_ns 从测量窗口开始统计，包含线程调度等待。时间字段只辅助定位停滞，定向 checker 不将有限时长样本解释为一般无饥饿证明。

## 尚未验证的边界与下一步

预生成计划依赖当前已知访问集 trace；排序、去重与存储分配在测量窗口前进行。之后做正式对比时必须披露这一点，不能把规划成本当作不存在。未知或依赖读取结果才能确定的访问集，以及 SIX 扩展，本轮未实现。根只按最终 IS/IX 申请，不在运行中做根 S/IX 转换；本轮不宣称通用 API 的任意跨层转换已全部验证。

FIFO 保证已获得本地资源 ticket 的请求不会被后来者超越，不等同于事务无饥饿，也不是 Wait-Die。NO_WAIT 和共享 QP 下的热点中止仍存在；同桶扫描加写采用保守 X，仍可能限制并发。

本轮没有跑新正式矩阵，也未做一小时压力验证，因此不将这次补丁称为全部实验验收完成。改动同时影响本地层级驱动和共享远端准入，旧正式曲线仅代表旧代码。后续由用户安排正式矩阵和长期压力测试，再更新图表与性能结论。
