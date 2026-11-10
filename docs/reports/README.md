# docs/reports

正式实验的报告与数据归档说明。图片产物在 `figures/formal/`，原始数据不入库。

## 报告清单

| 文件 | 内容 |
|---|---|
| `formal-matrix-report-2026-10-31.html` | 正式矩阵本地通路首跑：7 张图 + 汇总表 + 问题清单，单文件自包含（图片 base64 内嵌） |
| `range-lock-survey-2026-11-06.html` | 范围查询锁协议谱系调研：四类判据、十五个方案、五个实现落点、Citron 深读。单文件自包含，不含新实验数据 |
| `ycsb-e-skew-lines-2026-11-07.html` | YCSB-E-skew 正式矩阵折线图（7/7，343 主 case）：15 张 matplotlib 折线，单文件自包含（图片 base64 内嵌） |
| `ycsb-e-skew-bars-2026-11-07.html` | 同批数据的柱状/热图版：6 张图，单文件自包含 |
| `extended-lock-policies-lines-2026-11-10.html` | 扩展锁策略正式矩阵（dbf145d，427 case）：28 张 matplotlib 折线 + 对比结论，单文件自包含（图片 base64 内嵌） |

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

## 2026-11-10 扩展锁策略正式矩阵（dbf145d, 427 case）

- 完整可复现包：`docs/reports/extended-lock-policies-2026-11-10/`，含原始 HTML（相对引用 `assets/`）、28 张 PNG、2 份正式 CSV + 1 份 state JSON 与绘图脚本 `plot_extended.py`。顶层 `extended-lock-policies-lines-2026-11-10.html` 是同一报告的单文件自包含版（5.8 MB），便于单独分享。
- **数据例外**：按用户要求，本轮 427 个正式 case 的 2 份 CSV 与 state JSON 随报告归档；此前惯例是原始数据不入库。原始副本仍在 `.66:~/rrl-rolex-rdma/experiments/results/`，该路径被 `.gitignore` 排除。
- **数据来源与代码版本**：数据产自 `.66:~/rrl-rolex-rdma` 工作树（分支 `rolex-rdma-port-20261109`，HEAD `dbf145d`），campaign `20261109-175635-096373`。`dbf145d` 已在 main 祖先链上（main 比它多 6 个提交），配置 `experiments/configs/formal_lock_policies_extended_10s.json`；本轮以 `--mode both --resume` 续跑完成（起因是 1 个偶发 `workload failed` 与 `--mode local` 正确性门禁的既有缺陷），未改动仓库源码。
- 口径：`repetition_policy=single-run`、runs=1、seed=8、32 OS 线程、`execution_mode=threads` / `coroutines=1`、10s 预热 + 10s 测量；`zipf=0.99`，A/E 为 ordered-zipf、E-skew 为 hotspot 80/20；扫描宽度 8/32/128、桶宽 16/64/256、三种等待策略，9 种本地 + 8 种跨主机协议（含本轮首次进入正式矩阵的 `precision-lock`、`griffin-pl`）。**单次测量无误差线，不得宣称置信区间或运行间波动**。
- 规模：本地 217 + 跨主机 210 = **427 个正式 case**，state `status=complete`、`failed_cases=0`、`no_progress_cases=3`。3 个 0 提交格均为跨主机 `ycsb-e` + `wound-wait` + 大范围扫描，报告内以 `×` 标于对数轴底，不能当正常吞吐点。
- **口径警告（写综述必须遵守）**：CSV 的 `aborted` 是**尝试级**计数（`aborted ≈ retries`，wound-wait 下 `wound_aborts ≈ aborted`）；427 格 `logical_failed` 全为 0，没有一笔逻辑事务最终失败。因此 `aborted/(committed+aborted)` 只能叫「尝试级失败占比」，**不能写成事务中止率**。11-07 那轮「wound-wait 几乎消灭中止」是 θ=0.4 下同一口径，本轮 θ=0.99 下占比 87–95%，两轮数值不可混画。
- **结论要点**：键分布是隐藏自变量 —— ordered-zipf θ=0.99 下 `E-*` 吞吐掉到 10⁰–10²，换成 hotspot 的 `S-8` 回升到 10⁴–10⁵；落差来自「访问类型 × 键分布」，不是协议本身。跨主机每锁 RDMA 操作数 record 2PL 17.61 / exact-range 14.52 / hierarchical b64 8.91 / gap 3.00。详见报告第 5 节。
- 重跑：`python3 docs/reports/extended-lock-policies-2026-11-10/plot_extended.py`（需 matplotlib，读同目录 `data/`，一键重画 28 张图与 HTML 报告）。
