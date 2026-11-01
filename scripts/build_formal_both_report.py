#!/usr/bin/env python3
"""Build a self-contained HTML report for the 2026-11-01 local + cross-host matrices."""

import base64
import csv
import glob
import hashlib
import io
import os
import statistics as st
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RESULTS = os.path.join(ROOT, "experiments", "results")
FIGDIR = os.path.join(ROOT, "figures", "formal-both")
OUT = os.path.join(ROOT, "docs", "reports", "formal-matrix-2026-11-01.html")

THREADS = [1, 2, 4, 8, 16, 32]
LOCAL_CCS = ["none", "record-2pl", "gap-lock", "next-key", "hierarchical"]
ABORT_FIELDS = [("lock_conflicts", "conflicts"), ("lock_capacity_aborts", "capacity"),
                ("lock_gate_aborts", "gate")]


def latest(pattern):
    files = sorted(glob.glob(os.path.join(RESULTS, pattern)))
    if not files:
        raise SystemExit("missing " + pattern)
    with open(files[-1], newline="") as handle:
        return files[-1], list(csv.DictReader(handle))


def med(rows, field):
    return st.median(float(r[field]) for r in rows)


def idx(rows, keyfunc):
    out = defaultdict(list)
    for r in rows:
        out[keyfunc(r)].append(r)
    return out


LOCAL_PATH, local = latest("formal-local-2026*.csv")
CROSS_PATH, cross = latest("formal-cross-host-2026*.csv")
LOCAL_SHA = hashlib.sha256(open(LOCAL_PATH, "rb").read()).hexdigest()
CROSS_SHA = hashlib.sha256(open(CROSS_PATH, "rb").read()).hexdigest()

local_g = idx(local, lambda r: (r["workload"], r["cc"], int(r["bucket"]), int(r["threads"]), r["range"]))
cross_g = idx(cross, lambda r: (r["cc"], int(r["bucket"]), int(r["threads"]),
                                r["workload"], r["range"]))


def img(name):
    with open(os.path.join(FIGDIR, name), "rb") as handle:
        return "data:image/png;base64," + base64.b64encode(handle.read()).decode()


def local_table(workload, rng):
    rows = []
    for cc in LOCAL_CCS:
        b = 64 if cc == "hierarchical" else 0
        cells = []
        t1 = t32 = None
        for t in THREADS:
            key = (workload, cc, b, t, rng)
            if key not in local_g:
                cells.append("—")
                continue
            value = med(local_g[key], "throughput_committed_txn_s")
            cells.append(f"{value:,.0f}")
            if t == 1:
                t1 = value
            if t == 32:
                t32 = value
        rate = "—"
        key32 = (workload, cc, b, 32, rng)
        if key32 in local_g:
            rate = f"{100*med(local_g[key32],'aborted')/med(local_g[key32],'transactions'):.1f}%"
        drop = f"{t1/t32:.0f}×" if t1 and t32 else "—"
        rows.append((cc, cells, rate, drop))
    head = "<tr><th>cc</th>" + "".join(f"<th>t{t}</th>" for t in THREADS) + \
           "<th>中止率 @t32</th><th>t1→t32</th></tr>"
    body = "".join(
        f"<tr><td class=cc>{cc}</td>" + "".join(f"<td class=num>{c}</td>" for c in cells) +
        f"<td class=num>{rate}</td><td class=num>{drop}</td></tr>"
        for cc, cells, rate, drop in rows)
    return f"<div class=table-wrap><table>{head}{body}</table></div>"


def cross_table():
    series = [("rdma-next-key", 0, "rdma-next-key"),
              ("rdma-hierarchical", 16, "hier b16"),
              ("rdma-hierarchical", 64, "hier b64"),
              ("rdma-hierarchical", 256, "hier b256")]
    head = "<tr><th>cc</th>" + "".join(f"<th>t{t}</th>" for t in THREADS) + "</tr>"
    body = []
    for cc, b, label in series:
        cells = []
        for t in THREADS:
            key = (cc, b, t, "ycsb-e", "8")
            if key not in cross_g:
                cells.append("—")
                continue
            tput = med(cross_g[key], "throughput_committed_txn_s")
            rate = 100 * med(cross_g[key], "aborted") / med(cross_g[key], "transactions")
            cells.append(f"{tput:,.0f}<span class=sub>{rate:.0f}%</span>")
        body.append(f"<tr><td class=cc>{label}</td>" +
                    "".join(f"<td class=num>{c}</td>" for c in cells) + "</tr>")
    return f"<div class=table-wrap><table>{head}{''.join(body)}</table></div>"


def abort_table():
    agg = defaultdict(lambda: defaultdict(float))
    for r in cross:
        for field, _ in ABORT_FIELDS:
            agg[r["cc"]][field] += float(r[field])
        agg[r["cc"]]["transactions"] += float(r["transactions"])
        agg[r["cc"]]["aborted"] += float(r["aborted"])
    head = ("<tr><th>cc</th><th>事务数</th><th>中止</th><th>中止率</th>"
            "<th>conflicts</th><th>capacity</th><th>gate</th></tr>")
    body = []
    for cc, v in agg.items():
        body.append(
            f"<tr><td class=cc>{cc}</td><td class=num>{v['transactions']:,.0f}</td>"
            f"<td class=num>{v['aborted']:,.0f}</td>"
            f"<td class=num>{100*v['aborted']/v['transactions']:.1f}%</td>"
            f"<td class=num>{v['lock_conflicts']:,.0f}</td>"
            f"<td class=num>{v['lock_capacity_aborts']:,.0f}</td>"
            f"<td class=num>{v['lock_gate_aborts']:,.0f}</td></tr>")
    return f"<div class=table-wrap><table>{head}{''.join(body)}</table></div>"


FIGURES = [
    ("fig08_local_scaling_new_code.png", "图 1　本地扩展性（新代码）",
     "只有无锁基线是平的（YCSB-E 175k→160k，1.1×）。四个带锁协议全部单调下降，"
     "record-2pl / gap-lock / next-key 衰减 41~56 倍，而 t32 的中止率只有约 30% —— "
     "其余衰减来自每次事务结束都要拿满 16 片锁的全局屏障。"),
    ("fig11_local_abort_rate.png", "图 2　本地 YCSB-E 中止率",
     "上一轮四个协议在 YCSB-E 上全程 0% 中止（插入 key 落在扫描区间之外，幻读构造不出来）。"
     "本轮 t32 达到 29%~37%，说明插入已落进读者扫过的区间，v3/v4/v5 的差异第一次真的被压出来。"),
    ("fig09_cross_throughput.png", "图 3　跨主机：committed 平、attempted 涨",
     "左图提交吞吐在所有线程数下几乎水平；右图尝试吞吐随线程数上涨约 10 倍。"
     "两者之间的缺口就是被丢弃的工作。hierarchical b64 在 t16 的 313 txn/s 已查明是"
     "一次 1 秒 RDMA 超时把 elapsed 拉长（5000 事务 / 1.001s），不是性能劣化——"
     "全矩阵 8.3% 的 case 有同样指纹，与线程数和协议无关。"),
    ("fig10_cross_abort_composition.png", "图 4　跨主机中止构成",
     "左：rdma-next-key 的中止 100% 记为 conflicts，capacity 为 0。右：两个协议的中止率都严格落在 "
     "1 − 1/threads 上——每个线程只有第一个事务能提交，与 workload、range、zipf 全部无关。"),
    ("fig12_t1_local_vs_cross.png", "图 5　t=1 基线：本地 vs 跨主机",
     "同一 workload、同一协议、无并发无中止。本地 next-key 51,401 txn/s，跨主机 514 txn/s，"
     "差约 100 倍。这是传输与 gate 往返的成本，不是锁协议的差异。"),
]

PROBLEMS = [
    ("P1", "跨主机：每线程只能提交一个事务",
     "中止率 67.2%，逐线程严格等于 1 − 1/threads；两个协议完全一致",
     "committed = transactions / threads。每个线程第一个事务提交后，后续事务全部中止，"
     "且与 workload、range、zipf 无关 —— 这是协议实现问题，不是负载特征。"),
    ("P2", "跨主机：两种协议的中止来源完全不同",
     "rdma-next-key 362,828 次中止全部是 conflicts；rdma-hierarchical 1,088,370 次全部是 gate",
     "next-key：unlock_all 需要一次抢齐 16 个分片 gate，抢不到就整体放弃，锁静默残留，"
     "后续事务撞上残留锁被判冲突。hierarchical：根意向锁是 unbounded 区间，"
     "shard_ids 直接返回全部 16 片，每次根锁都要抢满所有门。同一病灶，两种表现。"),
    ("P3", "本地：unlock_all 一次拿满 16 片锁",
     "src/lock/lock_manager.cpp:111，all_shards() 后逐个 unique_lock；而 lock() 只取 1~3 片",
     "事务只有 10 个操作，结尾这个全局屏障占比极高。record-2pl 从 55,456 掉到 1,330（42×），"
     "但 t32 中止率只有 29% —— 大部分衰减来自这把锁，不是冲突。"),
    ("P4", "本地：无界区间把所有锁变成全局锁",
     "shard_ids(range) 对无界区间返回全部分片；v6 的根意向锁正是 unbounded()",
     "YCSB-A 上 hierarchical 从 161,450 掉到 489（330×），是所有组合里最差的。"
     "同一实现在 YCSB-E 上却是最快的（127,792→12,102）—— 排序在两个负载下完全相反，"
     "v6 那一节不能只引一条曲线。"),
    ("P5", "t=1 绝对性能差 100 倍，跨主机数据不可直接横比",
     "本地 next-key 51,401 vs 跨主机 514；本地 hier b64 127,792 vs 跨主机 2,469",
     "t=1 没有并发也没有中止，差距全部是单次加锁/解锁的往返成本。"
     "任何「RDMA 比本地慢 N 倍」的说法，测到的是协议实现的往返次数，不是 RDMA 的能力。"),
]


def build():
    cards = [
        ("1,188", "case 全部完成", "本地 756 + 跨主机 432，0 失败"),
        ("30 min", "本地矩阵耗时", "上一轮同样规模 2h45m"),
        ("67.2%", "跨主机中止率", "严格等于 1 − 1/threads"),
        ("0", "capacity 中止", "216 万事务下的锁槽溢出次数"),
    ]
    figs = "".join(
        f"<figure><figcaption>{title}</figcaption><img src=\"{img(name)}\" alt=\"{title}\">"
        f"<p class=caption>{caption}</p></figure>" for name, title, caption in FIGURES)
    probs = "".join(
        f"<div class=problem><div class=ph><span class=code>{code}</span>"
        f"<span class=ptitle>{title}</span></div><p class=where>{evidence}</p>"
        f"<p>{body}</p></div>" for code, title, evidence, body in PROBLEMS)

    html = f"""<!DOCTYPE html>
<html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>RdmaRangeLock 正式矩阵 · 本地 + 跨主机 · 2026-11-01</title>
<style>
:root {{ --bg:#fff; --fg:#1a1d21; --muted:#5f6b76; --line:#e3e7eb; --soft:#f6f8fa; --accent:#0b5fff; --warn:#b42318; }}
* {{ box-sizing:border-box; }}
body {{ margin:0; background:var(--bg); color:var(--fg);
  font:15px/1.7 -apple-system, BlinkMacSystemFont, "Segoe UI", "Microsoft YaHei", sans-serif; }}
main {{ max-width:1080px; margin:0 auto; padding:48px 28px 96px; }}
header {{ border-bottom:2px solid var(--fg); padding-bottom:20px; margin-bottom:32px; }}
h1 {{ font-size:27px; margin:0 0 10px; letter-spacing:-.3px; }}
.sub {{ color:var(--muted); font-size:13.5px; }}
.sub code {{ background:var(--soft); padding:1px 5px; border-radius:3px; font-size:12.5px; }}
h2 {{ font-size:19px; margin:52px 0 6px; letter-spacing:-.2px; }}
h2 .n {{ color:var(--muted); font-weight:400; margin-right:8px; }}
p {{ margin:10px 0; }}
.cards {{ display:grid; grid-template-columns:repeat(4,1fr); gap:14px; margin:26px 0 8px; }}
.card {{ border:1px solid var(--line); border-radius:8px; padding:16px 18px; background:var(--soft); }}
.card .v {{ font-size:25px; font-weight:650; letter-spacing:-.5px; }}
.card .l {{ font-size:13px; font-weight:600; margin-top:2px; }}
.card .d {{ font-size:12px; color:var(--muted); margin-top:4px; }}
.lead {{ border-left:3px solid var(--accent); padding:2px 0 2px 16px; margin:22px 0; color:#2b3138; }}
ul {{ padding-left:22px; }} li {{ margin:7px 0; }}
figure {{ margin:30px 0 10px; }}
figcaption {{ font-size:13.5px; font-weight:650; margin-bottom:8px; }}
figure img {{ width:100%; height:auto; border:1px solid var(--line); border-radius:6px; background:#fff; }}
.caption {{ font-size:13px; color:var(--muted); margin-top:8px; }}
.table-wrap {{ overflow-x:auto; margin:12px 0 4px; }}
table {{ border-collapse:collapse; width:100%; font-size:12.5px; }}
th, td {{ padding:7px 8px; border-bottom:1px solid var(--line); text-align:left; white-space:nowrap; }}
th {{ font-weight:600; color:var(--muted); font-size:11.5px; border-bottom:1.5px solid var(--line); }}
td.num {{ font-variant-numeric:tabular-nums; }}
td.cc {{ font-family:ui-monospace, Consolas, monospace; }}
.sub {{ font-size:10px; color:var(--muted); margin-left:5px; }}
h3 {{ font-size:14px; margin:26px 0 2px; }}
.problem {{ border:1px solid var(--line); border-radius:8px; padding:16px 18px; margin:14px 0; }}
.problem .ph {{ display:flex; align-items:baseline; gap:9px; }}
.code {{ display:inline-block; min-width:24px; text-align:center; background:var(--warn); color:#fff;
  font-size:12px; font-weight:700; border-radius:4px; padding:1px 6px; }}
.ptitle {{ font-weight:650; font-size:14.5px; }}
.problem p {{ font-size:13.5px; margin:7px 0; }}
.where {{ color:var(--muted); font-size:12.5px !important; font-family:ui-monospace, Consolas, monospace; }}
footer {{ margin-top:56px; padding-top:18px; border-top:1px solid var(--line);
  font-size:12.5px; color:var(--muted); word-break:break-all; }}
@media (max-width:780px) {{ .cards {{ grid-template-columns:repeat(2,1fr); }} main {{ padding:32px 16px 64px; }} }}
</style></head><body><main>
<header>
  <h1>RdmaRangeLock 正式矩阵：本地 + 跨主机</h1>
  <div class="sub">
    commit <code>a2c68f8</code> · 2026-10-31 18:43–19:38 UTC ·
    控制端 <code>.66</code>（Xeon Gold 5218 ×2, 64 线程, 125 GB）· RDMA 客户端 <code>.67</code><br>
    数据源 <code>{os.path.basename(LOCAL_PATH)}</code>（{len(local)} case）+
    <code>{os.path.basename(CROSS_PATH)}</code>（{len(cross)} case）
  </div>
</header>

<div class="cards">
{''.join(f'<div class=card><div class=v>{v}</div><div class=l>{l}</div><div class=d>{d}</div></div>' for v, l, d in cards)}
</div>

<h2><span class=n>01</span>一句话结论</h2>
<p class="lead">
本轮把两件事推进了：<b>锁槽容量不再是问题</b>（216 万事务下 capacity 中止为 0），
<b>幻读路径真的被打通了</b>（本地 YCSB-E 从全程 0% 中止变成 t32 的 30%）。
但同时也暴露了两个新的串行点：<b>跨主机的全分片 gate 获取</b>和
<b>本地 unlock_all 拿满 16 片锁</b>，它们让「每线程只提交一个事务」和
「带锁协议 t32 衰减 40~300 倍」成为本轮的主要现象。
</p>
<ul>
  <li><b>可用的</b>：t=1 的单客户端基线、锁数量对比、容量结论、幻读路径已生效这一事实。</li>
  <li><b>不可用的</b>：跨主机 t&gt;1 的任何吞吐数字、本地多线程的横向排序（都被全局解锁锁主导）。</li>
  <li><b>已查明</b>：hierarchical b64 在 t16 的 313 txn/s 是 1 秒 RDMA 超时，全矩阵 8.3% 的 case 命中同一指纹，跨主机吞吐需先剔除这些行。</li>
</ul>

<h2><span class=n>02</span>图表</h2>
{figs}

<h2><span class=n>03</span>本地汇总</h2>
<p class="note">三次重复取中位数，单位 committed txn/s；括号内为 t32 中止率。</p>
<h3>YCSB-E（range=8）</h3>
{local_table("ycsb-e", "8")}
<h3>YCSB-A（range=0）</h3>
{local_table("ycsb-a", "0")}

<h2><span class=n>04</span>跨主机汇总</h2>
<p class="note">YCSB-E range=8，格式为「committed txn/s（中止率）」。</p>
{cross_table()}
<h3>中止来源拆分</h3>
{abort_table()}

<h2><span class=n>05</span>遗留问题</h2>
{probs}

<h2><span class=n>06</span>复现</h2>
<ul>
  <li>本地：<code>scripts/run_formal_matrix.sh --mode local</code>（30 分钟）</li>
  <li>跨主机：<code>scripts/run_formal_matrix.sh --mode cross-host</code>（25 分钟，需 <code>.67</code> 与 deploy key）</li>
  <li>汇总：<code>scripts/analyze_formal_both.py</code>；出图：<code>scripts/plot_formal_both.py</code></li>
</ul>

<footer>
原始数据按 CONSTRAINTS 第四条不入库，校验和：<br>
local <code>{LOCAL_SHA}</code><br>
cross <code>{CROSS_SHA}</code>
</footer>
</main></body></html>
"""
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with io.open(OUT, "w", encoding="utf-8") as handle:
        handle.write(html)
    print("wrote", OUT, f"{os.path.getsize(OUT)/1024:.0f} KB")


if __name__ == "__main__":
    build()
