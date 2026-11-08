# docs/reports

正式实验的报告与数据归档说明。图片产物在 `figures/formal/`，原始数据不入库。

## 报告清单

| 文件 | 内容 |
|---|---|
| `formal-matrix-report-2026-10-31.html` | 正式矩阵本地通路首跑：7 张图 + 汇总表 + 问题清单，单文件自包含（图片 base64 内嵌） |
| `range-lock-survey-2026-11-06.html` | 范围查询锁协议谱系调研：四类判据、十五个方案、五个实现落点、Citron 深读。单文件自包含，不含新实验数据 |
| `ycsb-e-skew-lines-2026-11-07.html` | YCSB-E-skew 正式矩阵折线图（7/7，343 主 case）：15 张 matplotlib 折线，单文件自包含（图片 base64 内嵌） |
| `ycsb-e-skew-bars-2026-11-07.html` | 同批数据的柱状/热图版：6 张图，单文件自包含 |

## 数据归档

原始数据按 `CONSTRAINTS.md` 第四条不入库（`.gitignore` 忽略 `experiments/results/*.csv`），
存放在控制机 `.66` 上。重新出图前先核对校验和：

| 项 | 值 |
|---|---|
| 路径 | `.66:/home/dell/RdmaRangeLock/experiments/results/formal-local-20261031-024829.csv` |
| sha256 | `f203c79e5d4a813cfbb324ef6ad03c755694b0e86f4c01005559fb60ca34a003` |
| 大小 | 108,446 字节 |
| 行数 | 757（1 行表头 + 756 个 case） |
| 完成时间 | 2026-10-31 05:33:49 |
| 生成提交 | `b808549`（矩阵脚本）+ `3c20917`（CSV 校验修复） |
| 日志 | `experiments/logs/local-matrix.out`（756 个 completed，0 个 failed） |

校验：

```bash
cd /home/dell/RdmaRangeLock
sha256sum experiments/results/formal-local-20261031-024829.csv
# 期望 f203c79e5d4a813cfbb324ef6ad03c755694b0e86f4c01005559fb60ca34a003
```

## 重新生成

```bash
# 1. 出图（依赖见 scripts/requirements.txt）
scripts/plot_formal_matrix.py --mode local

# 2. 出报告（读最新一份 formal-local-*.csv，输出到 docs/reports/）
scripts/build_formal_report.py
```

两个脚本都只读 `experiments/results/` 与 `figures/formal/`，不会改动数据。

## 覆盖范围

本报告只覆盖 `--mode local` 的 756 个 case。跨主机 RDMA 的 432 个 case 尚未执行：
远程锁表现为每分片 1024 槽，并将容量不足单独记为 `lock_capacity_aborts`；跨主机矩阵仍需重新运行，不能沿用旧报告中受 64 槽上限污染的结论。详见 `docs/notes/formal-matrix-code-issues-2026-10-31.md` 的 C-4。

## 2026-11-07 YCSB-E-skew 正式矩阵（7/7）

- 完整可复现包：`docs/reports/ycsb-e-skew-2026-11-07/`，含两份原始 HTML（相对引用 `assets/`）、21 张 PNG、6 张 SVG、两个绘图脚本与 14 份正式 CSV。顶层两份为单文件自包含版本，便于单独分享。
- **数据例外**：按用户要求，本轮 343 个主 case 的 14 份正式 CSV 随报告归档；此前惯例是原始数据不入库。原始副本仍在 `.66:~/RdmaRangeLock/experiments/results/`，该路径被 `.gitignore` 排除。
- 口径：`repetition_policy=single-run`、runs=1、seed=8、32 OS 线程关闭协程、10s 预热 + 10s 测量；三种等待策略、桶宽 16/64/256、六种锁协议。**单次测量无误差线，不得宣称置信区间或运行间波动**。
- 七份配置各 25 本地 + 24 跨主机 = 49 case，共 343；日志 `ALL 7 CONFIGS DONE`，7 组 rc=0、`failed_cases=0`。
- 重跑：`python3 docs/reports/ycsb-e-skew-2026-11-07/plot_stage.py`（柱状/热图，纯标准库生成 SVG）与 `python3 docs/reports/ycsb-e-skew-2026-11-07/plot_lines_mpl.py`（折线，需 matplotlib）；脚本默认读同目录 `data/`，PNG 由浏览器或本机转换。
