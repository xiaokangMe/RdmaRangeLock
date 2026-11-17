#!/usr/bin/env python3
"""Compare one-worker and 32-worker runs of the same ARIES/KVL binary."""
import csv
import json
from pathlib import Path
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt


def main():
    single, multi = [Path(arg).resolve() for arg in sys.argv[1:3]]
    env1 = json.loads((single / "environment.json").read_text(encoding="utf-8"))
    env32 = json.loads((multi / "environment.json").read_text(encoding="utf-8"))
    assert env1["binary_sha256"] == env32["binary_sha256"], "different binaries"
    assert env1["host"] == env32["host"], "different machines"
    assert env1["config"]["threads"] == 1 and env32["config"]["threads"] == 32
    assert {k: v for k, v in env1["config"].items() if k != "threads"} == {k: v for k, v in env32["config"].items() if k != "threads"}, "other config differs"
    pairs = []
    rows = []
    for theta in env1["config"]["skews"]:
        pair = []
        row = {"theta": theta}
        for folder, n in [(single, 1), (multi, 32)]:
            r = json.loads((folder / ("theta-" + str(theta) + ".json")).read_text())
            w, m = r["warmup"], r["measurement"]
            assert r["threads"] == n and r["valid"] and r["metrics"]["live_lock_names"] == 0
            assert r["after_warmup_size"] == r["keys"] + w["committed_inserts"] + w["late_inserts"]
            assert r["final_size"] == r["after_warmup_size"] + m["committed_inserts"] + m["late_inserts"]
            assert m["errors"] == 0
            assert m["committed_scans"] + m["committed_inserts"] == m["commits"] * r["ops"]
            if n == 1:
                assert m["deadlocks"] == 0 and m["timeouts"] == 0 and r["metrics"]["lock_waits"] == 0
            for key in ["tps", "commits", "attempts", "deadlocks", "timeouts", "exhausted", "unfinished", "committed_inserts", "committed_scans", "committed_scan_rows"]:
                row[f"t{n}_{key}"] = m[key]
            row[f"t{n}_rows_per_commit"] = m["committed_scan_rows"] / m["commits"] if m["commits"] else 0
            row[f"t{n}_after_warmup_size"] = r["after_warmup_size"]
            row[f"t{n}_final_size"] = r["final_size"]
            row[f"t{n}_row_per_s"] = m["committed_scan_rows"] / m["window_s"]
            pair.append(r)
        assert pair[0]["trace_inserts"] == pair[1]["trace_inserts"]
        assert pair[0]["trace_operations"] == pair[1]["trace_operations"]
        row["speedup_32_over_1"] = pair[1]["measurement"]["tps"] / pair[0]["measurement"]["tps"] if pair[0]["measurement"]["tps"] else None
        pairs.append(pair); rows.append(row)
    with (single / "单线程与32线程对比.csv").open("w", encoding="utf-8-sig", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(rows[0]))
        writer.writeheader(); writer.writerows(rows)

    plt.rcParams["font.sans-serif"] = ["Microsoft YaHei", "Noto Sans CJK SC", "SimHei", "DejaVu Sans"]
    plt.rcParams["axes.unicode_minus"] = False
    fig, ax = plt.subplots(figsize=(10, 5.6), constrained_layout=True)
    x = [r["theta"] for r in rows]
    for n, color in [(1, "#d16b22"), (32, "#2563a6")]:
        y = [r[f"t{n}_tps"] for r in rows]
        ax.plot(x, y, "o-", linewidth=2, color=color, label=f"{n} 线程")
    ax.set_xticks(x, [str(theta) for theta in env1["config"]["skews"]])
    ax.set_xlabel("数据偏斜率（Zipf θ）")
    ax.set_ylabel("提交吞吐（事务/秒）")
    ax.set_title("新 B+ 树 ARIES/KVL：单线程与 32 线程\nYCSB-E，2,097,152 个初始键；预热 10 秒，测量 10 秒；每点一次")
    ax.set_ylim(bottom=0); ax.grid(alpha=.22); ax.legend()
    fig.savefig(single / "单线程与32线程对比.png", dpi=180)
    fig.savefig(single / "单线程与32线程对比.svg")
    plt.close(fig)

    lines = ["# ARIES/KVL 单线程与 32 线程对比", "",
        "在 `.66` 上使用相同的 Release 可执行文件运行。单线程九点是本轮新测；32 线程九点使用上一轮保存的实测值，没有重跑或覆盖。每点均只运行一次。", "",
        "## 参数与可比性", "",
        "- 相同参数：2,097,152 个初始键、9 个偏斜率、95% 扫描＋5% Insert、每事务 10 次操作、扫描逻辑宽度 32、seed=8、5,000 事务固定请求池、order=128、预热 10s＋测量 10s。",
        "- 使用相同的阻塞等待＋死锁检测，单次锁请求超时 100ms，最多重试 1,000 次，不额外退避。C++ 驱动和核心算法均未修改；只把工作线程由 32 改为 1。",
        "- 每个偏斜率重新装载初始树；预热后的提交插入保留到测量。单线程与 32 线程在预热期间提交的插入数量不同，因此测量开始时树内容和扫描返回行数存在差异。此对比可以观察单线程基线与并发退化，不能作为完全相同静态树上的纯线程扩展率。",
        "- 请求池相同，但线程数改变了请求分片、推进顺序和有限窗口内已完成的请求集合；95/5 是请求生成概率，已提交操作比例会受冲突及运行进度影响。",
        "- 未固定 CPU 亲和性，两轮先后运行；单轮测量不能证明小幅差异的统计显著性。", "",
        "## 结果", "",
        "| θ | 单线程 TPS | 32线程 TPS | 32线程/单线程 | 单线程中止 | 32线程中止 |",
        "|---:|---:|---:|---:|---:|---:|",
    ]
    for r in rows:
        lines.append(f'| {r["theta"]:g} | {r["t1_tps"]:,.1f} | {r["t32_tps"]:,.1f} | {r["speedup_32_over_1"]:.3f} | {r["t1_deadlocks"]+r["t1_timeouts"]:,} | {r["t32_deadlocks"]+r["t32_timeouts"]:,} |')
    lines += ["", "![TPS对比](单线程与32线程对比.png)", "", "## 实际扫描工作量", "",
        "返回行数只统计成功提交事务的扫描结果，不含失败尝试已扫描的行数。重复键追加、预热增长和已提交请求组成会影响每事务工作量。", "",
        "| θ | 单线程平均扫描行/提交事务 | 32线程平均扫描行/提交事务 | 单线程预热后记录数 | 32线程预热后记录数 |",
        "|---:|---:|---:|---:|---:|",
    ]
    for r in rows:
        lines.append(f'| {r["theta"]:g} | {r["t1_rows_per_commit"]:,.1f} | {r["t32_rows_per_commit"]:,.1f} | {r["t1_after_warmup_size"]:,} | {r["t32_after_warmup_size"]:,} |')
    lines += ["", "## 能确认与尚不能确认的内容", "",
        f'θ=0 时单线程 {rows[0]["t1_tps"]:,.1f} TPS，32 线程 {rows[0]["t32_tps"]:,.1f} TPS，单线程约快 {rows[0]["t1_tps"]/rows[0]["t32_tps"]:.1f} 倍。两轮此点都无死锁或超时中止，提交事务平均扫描行数接近，因此不能把低偏斜下的并发退化解释成大量事务中止。', "",
        f'单线程从 θ=0 到 θ=1.5，平均扫描行数由 {rows[0]["t1_rows_per_commit"]:,.1f} 行/提交事务增加到 {rows[-1]["t1_rows_per_commit"]:,.1f} 行/提交事务；吞吐由 {rows[0]["t1_tps"]:,.1f} 降至 {rows[-1]["t1_tps"]:,.1f} TPS。动态重复插入让热点范围包含越来越多记录，这说明高偏斜下还存在扫描工作量增长；仅凭这些数据不能量化它解释了多少吞吐下降。', "",
        "单线程测量阶段没有锁等待、死锁或锁超时，排除了其他工作事务造成的锁冲突；驱动逐行计时、游标调用和锁管理器的无竞争操作仍然存在。32 线程的中止数据来自同一核心实现的上一轮实测。", "",
        "这组对比不能单独证明论文协议有问题，也不能仅凭单线程更快就确定是某个函数调用或某把 mutex 的原因。定位原因还需要调用栈/CPU 与等待时间采样，并控制测量开始时的树内容和实际请求工作量。", "",
        "全部新测点通过树结构、最终记录数、提交操作数和锁释放校验。CSV 额外保存插入提交数、扫描返回行数、截窗未完成数及重试耗尽数。", "",
        "## 追溯", "",
        f'- 二进制 SHA256（两轮一致）：`{env1["binary_sha256"]}`。',
        f'- 本轮单线程原始目录：`.66:/home/dell/RdmaRangeLock-nextkey-paper-20261117/reports/{single.name}`。',
        f'- 上轮32线程原始目录：`.66:/home/dell/RdmaRangeLock-nextkey-paper-20261117/reports/{multi.name}`。',
        "- 单线程重跑命令：`python3 -u scripts/benchmark_aries_kvl_ycsbe.py --threads 1`。",
        "- 所有原始命令、JSON、日志和环境信息均保存在各自目录。目录时间取自远端系统时钟。", "",
    ]
    (single / "单线程对比报告.md").write_text("\n".join(lines), encoding="utf-8")
    print("同二进制、配置及数据校验通过，已输出：", single)
    for r in rows:
        print(f'{r["theta"]:g}: single={r["t1_tps"]:.1f}, 32-thread={r["t32_tps"]:.1f}, ratio={r["speedup_32_over_1"]:.3f}')


if __name__ == "__main__":
    main()
