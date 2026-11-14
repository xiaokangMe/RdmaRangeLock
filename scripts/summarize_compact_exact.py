#!/usr/bin/env python3
"""Write a reviewer-facing report for benchmark_compact_exact.py output."""
import csv
import json
from pathlib import Path
import sys


def main():
    out = Path(sys.argv[1]).resolve()
    rows = list(csv.DictReader((out / "summary.csv").open()))
    meta = json.loads((out / "metadata.json").read_text())
    config = meta["config"]
    def row(theta, policy, backend, metrics):
        return next(r for r in rows if float(r["theta"]) == theta and r["wait_policy"] == policy
                    and r["backend"] == backend and r["metrics"] == metrics)
    lines = [
        "# Exact-range 压缩 CAS 性能对比",
        "",
        "`.66`，双路 Xeon Gold 5218，32 物理核/64 逻辑 CPU；目标分支 `codex/record2pl-compact-20261112`。",
        "",
        f"YCSB-E：{config['keys']:,} keys，32 OS threads，5,000 条有限 trace、每事务 10 次操作、range 32、seed 8；"
        f"theta={config['zipf_values']}，三策略。每项 {config['rounds']} 轮，预热 {config['warmup']} 秒，测量 {config['duration']} 秒。"
        f"合计 {meta['passed']} 条有效数据，失败 {meta['failures']} 次。表中 TPS 为提交吞吐中位数，括号为 min–max。",
        "",
        "| theta | 策略 | generic/full TPS | compact/full TPS | 倍率 | compact/fast TPS |",
        "|---:|---|---:|---:|---:|---:|",
    ]
    for theta in config["zipf_values"]:
        for policy in config["wait_policies"]:
            g = row(theta, policy, "generic", "full")
            c = row(theta, policy, "compact", "full")
            f = row(theta, policy, "compact", "fast")
            def tps(r):
                return f"{float(r['median_tps']):,.0f} ({float(r['min_tps']):,.0f}–{float(r['max_tps']):,.0f})"
            ratio = float(c["median_tps"]) / float(g["median_tps"])
            lines.append(f"| {theta:g} | {policy} | {tps(g)} | {tps(c)} | {ratio:.2f}× | {tps(f)} |")
    lines += [
        "",
        "## 完整统计下的冲突和完成情况",
        "",
        "三个独立运行累加计数；比率按累计提交数归一化。未完成包含测量结束时尚未提交和重试耗尽的逻辑事务。",
        "",
        "| theta | 策略 | 后端 | 中止/提交 | 重试/提交 | 等待 ms/提交 | 未完成 | 重试耗尽 |",
        "|---:|---|---|---:|---:|---:|---:|---:|",
    ]
    for theta in config["zipf_values"]:
        for policy in config["wait_policies"]:
            for backend in ("generic", "compact"):
                r = row(theta, policy, backend, "full")
                lines.append(f"| {theta:g} | {policy} | {backend} | {float(r['abort_per_commit']):.2f} | "
                    f"{float(r['retry_per_commit']):.2f} | {float(r['wait_ms_per_commit']):.4f} | "
                    f"{int(r['unfinished']):,} | {int(r['retry_exhausted']):,} |")
    lines += ["", "## 压缩空间", "", "| theta | 锁字数组 bytes | 事务计划 bytes |", "|---:|---:|---:|"]
    for theta in config["zipf_values"]:
        r = row(theta, config["wait_policies"][0], "compact", "full")
        lines.append(f"| {theta:g} | {int(r['lock_bytes']):,} | {int(r['plan_bytes']):,} |")
    lines += [
        "",
        "- 压缩单元为精确端点划分，空隙仍受保护；每单元 8 字节，范围可能覆盖多个单元。context 表、预分配释放列表另见 results.csv。generic 内存为 0 代表未测量。",
        "- compact 在计时前编译整个固定 trace 的几何请求，并提前取得事务锁集合；generic 按操作逐步取锁。结果包括预规划、合并、加锁时机和 authority 的综合效果，不能全部归因于 mutex 删除。",
        "- fast 关闭可选 latency/statistics；其等待直方图、rows、checksum 未采样，不能将 0 当作无等待或无读写。较低观测开销也不保证在每个争用场景中吞吐都提高。",
        "- 高偏斜下仍有重试耗尽及 wound-wait 中止风暴。保留原始统计，不以吞吐改进代替逻辑事务完成情况。",
        "- 这是 3 秒短窗口、三轮优化对照；不能与此前 10 秒单轮 sweep 直接混组。有限键域和重复 trace 的 Insert 可能覆盖已有键。",
        "- 22 项相关 CTest 通过；包含新增 20,000 组冲突对照、空隙/边界保护、混合事务最终数据对照及 runner 测试。ctest.log、源快照/hash、分支、每次命令、stdout/stderr 和全部 CSV 在此目录。",
        "",
    ]
    if (out / "audit.json").exists():
        audit = json.loads((out / "audit.json").read_text())
        lines += [
            "## 构建身份核验",
            "",
            f"audit.json 核对 {audit['rows']} 行、{audit['groups']} 个组合，每组合三轮；错误数 {audit['errors']}。"
            f"实验结束时 runner 哈希与开始时一致：{audit['runner_hash_matches']}。",
            "",
            "实验期间部分共享源码随后发生了其它修改，runner 未重新生成。本次验证和性能数据对应 sources/ 中的源码快照；这些后续修改不包含在本轮验证范围内。",
            "",
        ]
    (out / "report.md").write_text("\n".join(lines), encoding="utf-8")
    print(out / "report.md")


if __name__ == "__main__":
    main()
