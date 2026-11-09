# Citron / ROLEX one-hour correctness soak

- 日期：2026-11-09T09:03:56.563168+00:00 至 2026-11-09T10:03:57.580082+00:00
- 类型：本地并发正确性压力测试，不是正式性能矩阵
- 代码基线：工作树包含既有未提交 Griffin 改动；精确内容见 `source-manifest.json`
- CTest：默认构建 `64/64` 测试通过；无 RDMA 环境的 20 个测试按 `SKIP_RETURN_CODE` 跳过

| 案例 | 时长 | 结果 | 关键输出 |
|---|---:|---|---|
| Citron integration | 3600 s | PASS | grants=10,356,948; violations=0; errors=0 |
| ROLEX learned-index soak | 3600 s | PASS | operations=15,662,011,938; errors=0; valid=1 |

日志目录：`/home/dell/RdmaRangeLock/experiments/logs/citron-rolex-soak-20261109T090356Z`。

Citron 测试覆盖本地范围互斥、事务自重叠扩展、失败封闭、释放清理、随机祖先/后代并发和锁泄漏检查。ROLEX 测试覆盖 8 线程点查、插入、扫描、叶分裂与最终 `validate()`。

该结果只证明当前适配代码在这组本地压力路径下没有观察到正确性错误；Citron 跨主机 RDMA 和 ROLEX 真实远端 learned-index 路径仍需单独验证，不能把本报告当成论文性能复现。
