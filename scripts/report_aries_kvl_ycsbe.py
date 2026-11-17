#!/usr/bin/env python3
"""Validate collected results and create a separate Chinese report/plot."""
import json
from pathlib import Path
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt


def main():
    folder = Path(sys.argv[1]).resolve()
    env = json.loads((folder / "environment.json").read_text(encoding="utf-8"))
    config = env["config"]
    results = []
    for theta in config["skews"]:
        r = json.loads((folder / ("theta-" + str(theta) + ".json")).read_text())
        w, m = r["warmup"], r["measurement"]
        assert r["valid"] and r["metrics"]["live_lock_names"] == 0
        assert r["after_warmup_size"] == r["keys"] + w["committed_inserts"] + w["late_inserts"]
        assert r["final_size"] == r["after_warmup_size"] + m["committed_inserts"] + m["late_inserts"]
        for phase in (w, m):
            assert phase["errors"] == 0
            assert phase["committed_scans"] + phase["committed_inserts"] == phase["commits"] * r["ops"]
            assert abs(phase["tps"] - phase["commits"] / phase["window_s"]) < 1e-5
        results.append(r)

    plt.rcParams["font.sans-serif"] = ["Microsoft YaHei", "Noto Sans CJK SC", "SimHei", "DejaVu Sans"]
    plt.rcParams["axes.unicode_minus"] = False
    fig, ax = plt.subplots(figsize=(10, 5.4), constrained_layout=True)
    x = [r["theta"] for r in results]
    y = [r["measurement"]["tps"] for r in results]
    ax.plot(x, y, "o-", color="#2563a6", linewidth=2, markersize=6, label="ARIES/KVL（论文协议内存实现）")
    ax.set_xticks(x, [str(theta) for theta in config["skews"]])
    ax.set_xlabel("数据偏斜率（Zipf θ）")
    ax.set_ylabel("提交吞吐（事务/秒）")
    ax.set_title(f'新 B+ 树 ARIES/KVL：YCSB-E，{config["threads"]} 线程，{config["keys"]:,} 个初始键\n95% 扫描 + 5% 插入；预热 {config["warmup"]} 秒，测量 {config["duration"]} 秒；每点一次')
    ax.set_ylim(bottom=0)
    ax.grid(alpha=.22)
    ax.legend(loc="best")
    for theta, tps in zip(x, y):
        ax.annotate(f"{tps:,.1f}", (theta, tps), xytext=(0, 9), textcoords="offset points", ha="center", fontsize=9)
    fig.savefig(folder / "ycsbe-tps.png", dpi=180)
    fig.savefig(folder / "ycsbe-tps.svg")
    plt.close(fig)

    lines = [
        "# 新 B+ 树 ARIES/KVL：YCSB-E 单机实测", "",
        "本次只测 `codex/next-key-paper-20261117` 的独立 `rrl_aries_kvl`，没有使用旧 compact 执行器，也没有改动旧七算法图。", "",
        "## 实验设置", "",
        f'- 机器：`.66`，Xeon Gold 5218 双路，32 个物理核 / 64 个逻辑 CPU；工作线程 {config["threads"]}；不固定线程亲和性。',
        f'- Release 构建；单进程、纯内存 B+ 树；每点重新创建索引并插入 {config["keys"]:,} 条初始记录。初始装载不计入预热或测量。',
        f'- YCSB-E：每事务 {config["ops"]} 次操作，每次以 95% 概率扫描、5% 概率插入；扫描宽度为 {config["range-length"]} 个逻辑整数键区间，而非最多返回该数量的记录。',
        "- 保留 Deneva Insert 行为：初始键编码为 rank×10000，插入键为 anchor×10000+四位小数；重放同键时追加新 RID，真正修改叶页并允许分裂，不做 upsert。",
        f'- 每点预生成 {config["trace-txns"]:,} 个事务，seed={config["seed"]}，每个事务内 anchor 不重复，按线程分片循环重放；θ=0 使用均匀分布，其他点使用有序 Zipf CDF。95/5 是生成概率，实际 trace 比例保存在各点 JSON。',
        f'- 每点预热 {config["warmup"]} 秒；预热结束全部工作线程停止、清理在途事务，然后保留同一棵树开始 {config["duration"]} 秒测量。预热提交的新增记录保留，故测量起始行数不完全相同。',
        f'- 锁冲突处理为当前新实现的阻塞等待＋死锁检测，单次请求超时 {config["lock-timeout-ms"]}ms，事务从头重试，最多 {config["max-attempts"]:,} 次；不另加退避。不是旧版 no-wait / wait-die / wound-wait 三策略矩阵。',
        f'- B+ 树 order={config["order"]}，按逻辑 RID 条目数量决定页容量；锁管理器仍用 map＋mutex/CV，这是当前原型的实现选择。',
        "- 一点一次，不能据此判断统计显著性，也不绘制均值/最小值/最大值误差线。", "",
        "## 测量口径", "",
        f'TPS = 在测量截止前完成提交的事务数 / {config["duration"]} 秒。跨过截止时刻的提交另计 late_commits；截止时仍在途的逻辑事务回滚，另计 unfinished，不算重试耗尽。清理耗时 drain_s 单独保存，不混入 TPS 分母。deadlocks/timeouts 是失败尝试数；exhausted 是达到重试上限后放弃的逻辑事务数。锁请求、分裂和 undo 指标包含该阶段末尾的清理。', "",
        "每点均核对：树结构 validate 通过、最终行数 = 初始行数 + 预热和测量阶段已提交插入数（包括 late 提交）、所有锁最终释放、已提交操作数 = 已提交事务数×10。", "",
        "## 结果", "",
        "| 偏斜率 θ | 提交 TPS | 提交事务 | 死锁中止 | 超时中止 | 重试耗尽 | 截窗未完成 | 测量提交插入 | 最终记录数 |",
        "|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for r in results:
        m = r["measurement"]
        lines.append(f'| {r["theta"]:g} | {m["tps"]:,.1f} | {m["commits"]:,} | {m["deadlocks"]:,} | {m["timeouts"]:,} | {m["exhausted"]:,} | {m["unfinished"]:,} | {m["committed_inserts"]:,} | {r["final_size"]:,} |')
    lines += ["", "![提交吞吐折线图](ycsbe-tps.png)", "", "## 验证与提交组成", "",
        "正式运行前，Release 协议回归测试通过；独立驱动分别以 1 线程均匀分布、32 线程 θ=1.5 做了小规模运行，确认真实插入、回滚和最终记录数统计。正式九点逐点运行上述数据与锁释放检查，均通过。",
        "95/5 指请求的生成概率，并不保证在有限窗口内已提交事务仍保持相同比例。发生冲突的事务会重试或停留在途，不能用总 TPS 代替写入提交进度。下表同时列出原始请求池和测量期间已提交操作的插入比例。", "",
        "| θ | 请求池插入比例 | 已提交操作中插入比例 | 测量中止尝试数 | 重试耗尽 |",
        "|---:|---:|---:|---:|---:|",
    ]
    for r in results:
        m = r["measurement"]
        committed_ops = m["committed_scans"] + m["committed_inserts"]
        ratio = f'{100*m["committed_inserts"]/committed_ops:.2f}%' if committed_ops else "无提交"
        lines.append(f'| {r["theta"]:g} | {100*r["trace_inserts"]/r["trace_operations"]:.2f}% | {ratio} | {m["deadlocks"]+m["timeouts"]:,} | {m["exhausted"]:,} |')
    lines += ["", "## 解释范围", "",
        "这里的扫描通过新实现的 open/next 直接执行论文的 Fetch/FetchNext 路径：先对真实叶页加 latch，定位当前 key/successor，按 KVL 请求事务锁，冲突时释放页 latch 再等待、验证。事务锁状态保存在独立锁管理器中，并不是把锁字直接嵌入叶页；这两者必须区分。",
        "本实验测量的是这套原型的实际提交吞吐。高偏斜会使扫描和插入集中在相同 key/范围；是否出现死锁或超时可由本表确认。吞吐变化同时受锁管理器串行部分、页 latch、扫描新增重复记录、事务重试等因素影响；本轮没有采样剖析，不能把曲线变化仅归因于某一个因素。",
        "协议依据为 Mohan 的 ARIES/KVL（VLDB 1990）；原文没有这组 YCSB-E 实验。这里不是原文 TPS 的数值复现，也不能与采用不同插入语义、索引或事务组成的旧曲线直接拼接。", "",
        "## 文件与重跑", "",
        "- `summary.csv`：逐点结果汇总；`theta-*.json`：完整原始结果；`theta-*.stderr.log`：装载和阶段日志；`theta-*.command.json`：完整命令。",
        "- `environment.json`：CPU/编译器、配置、分支、基线提交、可执行文件 SHA256；`source.patch` 与源文件快照保存当时的 benchmark 代码。",
        "- `scripts/benchmark_aries_kvl_ycsbe.py` 的 CONFIG 集中设置所有可变参数；同一个脚本一次顺序运行九点。",
        "- 正式构建目标：`cmake -S . -B build-aries-release -DCMAKE_BUILD_TYPE=Release`，然后 `cmake --build build-aries-release --target rrl_aries_kvl_runner -j8`。",
        "- 运行：`python3 -u scripts/benchmark_aries_kvl_ycsbe.py`；生成图表：`python3 scripts/report_aries_kvl_ycsbe.py <结果目录>`。", "",
        f'远端原始目录：`/home/dell/RdmaRangeLock-nextkey-paper-20261117/reports/{folder.name}`。目录时间取自远端系统时钟；以 environment.json 的主机、源码和二进制 hash 追溯，不以目录日期判断代码版本。', "",
        f'基线提交：`{env["git_head"]}`；二进制 SHA256：`{env["binary_sha256"]}`。', "",
    ]
    (folder / "实验报告.md").write_text("\n".join(lines), encoding="utf-8")
    print("已验证全部九点，已生成中文报告与折线图：", folder)


if __name__ == "__main__":
    main()
