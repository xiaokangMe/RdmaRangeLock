#!/usr/bin/env python3
"""Summarize lookup/profile raw CSV with all repetitions retained."""
import argparse
import collections
import csv
import statistics
from pathlib import Path


def span(values, digits=2):
    return f"{statistics.median(values):.{digits}f} [{min(values):.{digits}f}, {max(values):.{digits}f}]"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lookup", required=True)
    parser.add_argument("--profile", required=True)
    args = parser.parse_args()
    lookup = list(csv.DictReader(Path(args.lookup).open()))
    profile = list(csv.DictReader(Path(args.profile).open()))
    commits = {r["source_commit"] for r in lookup + profile}
    if len(commits) != 1:
        raise SystemExit("mixed source commits; analyze each separately")
    print("# 查询策略与 next-key 定向测量（2026-11-02）\n")
    print("测量源码：" + next(iter(commits)) + "。")
    print("配置：experiments/configs/slot-lookup-bench.json、experiments/configs/rdma-lookup-profile.json。")
    print("原始数据（.66，忽略入库）：" + args.lookup + "、" + args.profile + "，同名 JSON 保存环境。")
    print("报告生成：scripts/analyze_rdma_lookup.py；所有点均三次重复，以下是中位数 [最小值, 最大值]。\n")
    print("## 完整快照查询微基准\n")
    print("每次都做 build + 全部冲突候选判定 + 精确 owned 查找；不跨请求复用索引。")
    print("两种策略 checksum 一致。每点 300 次，少量槽时窗口很短且 CPU 频率/调度噪声明显；不能据小差异推导通用阈值。\n")
    print("| 槽数 | 分布 | index ns/次 | linear ns/次 | index/linear 中位数比 |")
    print("|---:|---|---:|---:|---:|")
    groups = collections.defaultdict(lambda: collections.defaultdict(list))
    for row in lookup:
        groups[int(row["slots"]), row["shape"]][row["policy"]].append(float(row["ns_per_lookup"]))
    for (count, shape), values in sorted(groups.items()):
        a, b = values["index"], values["linear"]
        if len(a) != 3 or len(b) != 3:
            raise SystemExit("expected three repetitions for both policies")
        print(f"| {count} | {shape} | {span(a, 1)} | {span(b, 1)} | {statistics.median(a)/statistics.median(b):.3f} |")
    print("\n## 跨主机诊断\n")
    print(".66(10.0.0.3) 客户端 → .67(10.0.0.4) 隔离服务端；两侧源码一致，GID=3。")
    print("YCSB-E，256 keys、1,000 事务、4 ops/txn、range=16、4 threads、2 QP、bucket=64、seed=7。")
    print("固定 local gate=1、gate retry=64、profile=1；两策略交替顺序，其余相同。")
    print("保守全 key 域驻留界为 4×4×(256+2)=4,128 槽/shard，小于 8,192；全部测量容量/硬错误为零。")
    print("单次仅约 0.7–0.9 秒，含 profiling 开销，属于诊断，不能替代至少 10 秒正式窗口。\n")
    print("| 策略 | committed txn/s | 成功事务数 | elapsed ms | sync/attempt | data KiB/attempt |")
    print("|---|---:|---:|---:|---:|---:|")
    profiles = collections.defaultdict(list)
    for row in profile:
        profiles[row["lookup_policy"]].append(row)
    for policy, rows in sorted(profiles.items()):
        if len(rows) != 3:
            raise SystemExit("expected three profile repetitions")
        def values(key, scale=1):
            return [float(row[key]) / scale for row in rows]
        sync = [int(row["rdma_sync_calls"])/int(row["transactions"]) for row in rows]
        data = [(int(row["rdma_read_bytes"])+int(row["rdma_write_bytes"]))/int(row["transactions"])/1024 for row in rows]
        print(f"| {policy} | {span(values('throughput_committed_txn_s'))} | {span(values('committed'),0)} | {span(values('elapsed_ns',1e6))} | {span(sync)} | {span(data)} |")
    print("\n| 策略 | build ms | conflict+owned ms | QP io wait ms | local gate wait ms | gate held ms |")
    print("|---|---:|---:|---:|---:|---:|")
    for policy, rows in sorted(profiles.items()):
        cells = [span([int(r["rdma_"+key])/1e6 for r in rows]) for key in
                 ["index_build_ns","lookup_ns","io_wait_ns","local_gate_wait_ns","gate_hold_ns"]]
        print("| " + policy + " | " + " | ".join(cells) + " |")
    print("\n以上 wait/hold 为所有线程的累计量，可以超过墙钟时间；io wait 是软件 QP 使用互斥等待，不能解释成 NIC 队列深度。")
    print("sync_calls 是同步 post/poll 提交代理，不是物理 RTT；data 只含 READ/WRITE 载荷，不含 CAS 和网络协议开销。")
    print("每次尝试约 230 次同步提交，仍有明显的 next-key 区间逐条请求成本；两策略均没有减少逻辑锁区间。")
    a = statistics.median(float(r["throughput_committed_txn_s"]) for r in profiles["index"])
    b = statistics.median(float(r["throughput_committed_txn_s"]) for r in profiles["linear"])
    print(f"\n该组短诊断 linear 的成功吞吐中位数比 index 高 {(b/a-1)*100:.1f}%；只用于实现选择，不声称正式矩阵提升。")
    print("\n## 决策与后续\n")
    print("默认采用 linear，保留 RRL_SLOT_LOOKUP=index 供单变量消融。理由是后端每次重新读取槽表，只执行一组查询，无法摊销排序；此轮完整成本和实际诊断支持线性默认。")
    print("没有建立可复用的远端版本快照，也没有证据支持按固定槽数切换；暂不引入任意阈值。")
    print("下一步先设计减少重复槽表读取/批量执行独立 next-key 请求的协议，逐条保留原区间、模式、冲突返回和 strict 2PL；跨逻辑请求的 batch 尚未实现。")
    print("之后做无 profiling / 有 profiling 的开销对照、至少 10 秒且三次重复的正式矩阵。完整跨主机矩阵需重新按其配置核算槽容量。")
    print("该测量源码早于后续同事务跨 QP 的发布/记账序列化修正；最终代码正式基线仍须重测，不能直接沿用这里的数值。")
    print("本轮未做 1 小时稳定性压力；不能据短回归认定压力验收完成。")


if __name__ == "__main__":
    main()
