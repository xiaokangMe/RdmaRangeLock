#!/usr/bin/env python3
"""Build a self-contained HTML report from the formal matrix CSV + figures."""

import base64
import csv
import glob
import hashlib
import io
import os
import statistics as stats
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIG_DIR = os.path.join(ROOT, "figures", "formal")

_csv = sorted(glob.glob(os.path.join(ROOT, "experiments", "results", "formal-local-*.csv")))
if not _csv:
    raise SystemExit("no experiments/results/formal-local-*.csv found")
CSV_PATH = _csv[-1]
_stamp = os.path.basename(CSV_PATH)[len("formal-local-"):-len(".csv")]
_day = _stamp.split("-")[0]
_day = f"{_day[:4]}-{_day[4:6]}-{_day[6:8]}"
OUT_PATH = os.path.join(ROOT, "docs", "reports", f"formal-matrix-report-{_day}.html")

DIGEST = hashlib.sha256(open(CSV_PATH, "rb").read()).hexdigest()
SIZE = os.path.getsize(CSV_PATH)
SHORT_DIGEST = DIGEST[:16]

THREADS = [1, 2, 4, 8, 16, 32]
CCS = ["none", "record-2pl", "gap-lock", "next-key", "hierarchical"]
WL = ["ycsb-a", "ycsb-e", "tpcc-payment", "tpcc-new-order"]
WL_LABEL = {
    "ycsb-a": "YCSB-A（50% 更新）",
    "ycsb-e": "YCSB-E（范围读 + 插入）",
    "tpcc-payment": "TPC-C payment",
    "tpcc-new-order": "TPC-C new-order",
}

rows = list(csv.DictReader(open(CSV_PATH, newline="")))
table = defaultdict(list)
for r in rows:
    table[(r["workload"], r["cc"], int(r["bucket"]), int(r["range"]), int(r["threads"]))].append(r)


def med(key, field):
    group = table.get(key)
    if not group:
        return None
    return stats.median(float(x[field]) for x in group)


def bucket_of(workload, cc):
    if cc != "hierarchical":
        return 0
    for b in (64, 16, 256):
        if (workload, cc, b, 0 if workload != "ycsb-e" else 8, 1) in table:
            return b
    return 64


def img(name):
    path = os.path.join(FIG_DIR, name)
    data = base64.b64encode(open(path, "rb").read()).decode()
    return f"data:image/png;base64,{data}"


def fmt(v, digits=0):
    if v is None:
        return "—"
    return f"{v:,.{digits}f}"


def summary_table(workload):
    rng = 8 if workload == "ycsb-e" else 0
    out = []
    for cc in CCS:
        b = bucket_of(workload, cc)
        key = lambda t: (workload, cc, b, rng, t)
        t1, t4, t32 = (med(key(t), "throughput_committed_txn_s") for t in (1, 4, 32))
        ab1 = med(key(1), "aborted") / med(key(1), "transactions") * 100
        ab32 = med(key(32), "aborted") / med(key(32), "transactions") * 100
        drop = t1 / t32 if t1 and t32 else 0
        out.append((cc, b, t1, t4, t32, ab1, ab32, drop))
    return out


def bar(value, reference, color):
    width = max(1.0, 100.0 * value / reference) if reference else 0
    return (f'<div class="bar"><span style="width:{width:.1f}%;background:{color}"></span></div>')


CC_COLOR = {
    "none": "#7f7f7f",
    "record-2pl": "#1f77b4",
    "gap-lock": "#ff7f0e",
    "next-key": "#2ca02c",
    "hierarchical": "#d62728",
}

figures = [
    ("fig01_throughput_vs_threads.png", "图 1　吞吐 vs 线程数",
     "四个 workload 各一条曲线组。所有锁协议随线程数单调下降，无一随核数上升——"
     "说明这一轮测到的是互斥锁排队，不是并发扩展。hierarchical 在 YCSB-E 上最高、在 YCSB-A 上最低，"
     "两者必须成对阅读。"),
    ("fig02_abort_ratio_vs_threads.png", "图 2　no-wait 中止率",
     "YCSB-A 在 t2 就跳到 43%，t32 达 88~89%；hierarchical 从 t2 起就在 80% 以上，t32 到 99%。"
     "两个 TPC-C 负载除 hierarchical 外全程 0%；YCSB-E 所有线程数、所有协议均为 0%。"),
    ("fig07_wasted_work.png", "图 3　attempted vs committed",
     "浅色三角线是 attempted，深色圆点线是 committed。YCSB-A t32：record-2pl 35,388 → 3,740，"
     "hierarchical 40,498 → 421。而且 attempted 自身也随线程下降，说明瓶颈在锁表串行化，不是重试过多。"),
    ("fig03_range_length_effect.png", "图 4　YCSB-E 范围长度的影响",
     "range 从 8 到 128，逐行加锁的三档在 4 线程下从数千掉到约 70 txn/s；"
     "hierarchical 因为锁数量不随范围增长，仍保持约 4,000，相差约 60 倍。"),
    ("fig04_lock_and_scan_cost.png", "图 5　锁数量与每行锁成本",
     "左：range=128 时逐行协议要 610 万次加锁，hierarchical 几乎不变。"
     "右：折算到「每返回一行付几把锁」，hierarchical 为 0.03~0.4，其余为 1.0~1.38。"),
    ("fig05_bucket_sweep.png", "图 6　bucket 宽度扫描",
     "两条曲线的排序完全相反：YCSB-E（扫描为主）256 > 64 > 16，YCSB-A（写冲突为主）16 > 64 > 256。"
     "矩阵里固定 64 是折中值，两个场景都不占优。"),
    ("fig06_single_thread_overhead.png", "图 7　单线程开销",
     "唯一不受互斥排队干扰的对比维度。YCSB-E 上 hierarchical（150.7k）比 next-key（58.7k）快 2.6 倍，"
     "因为一把桶锁顶替了约 33 把行锁；但在 YCSB-A 上五者的差距收窄到 20% 以内。"),
]

problems = [
    ("A", "bucket 宽度的最优值随负载反转，固定值无法两用",
     "fig05；`include/lock/remote_lock_manager.h` / `src/txn/workload.cpp`",
     "YCSB-E 上 256 > 64 > 16，YCSB-A 上 16 > 64 > 256，两条曲线完全反序。"
     "矩阵固定 bucket=64，两类负载都不占优。",
     "v6 的结论不能写成「层级锁更好」，必须绑定 bucket 宽度与负载类型；"
     "把 bucket 做成按 workload 可配，并把 16/64/256 三条完整曲线都跑出来。"),
    ("B", "YCSB-E 的插入永远落在扫描区间之外，幻读防护未被测量",
     "`src/txn/workload.cpp` 中的 `ycsb()`",
     "插入 key = `key_count + n*ops + i`（恒 ≥ key_count），范围读上界 ≤ key_count，两者系统性不相交。"
     "实测 YCSB-E 全线程、全协议 aborted 均为 0。",
     "v3（只锁行）在这一轮不会产生幻读，v4/v5 防住的幻读本来就不存在；"
     "三者的差别只剩锁数量。项目的核心命题仍然悬空。"),
    ("C", "无锁基线自己也随线程下降 3~6 倍，索引层是独立瓶颈",
     "`include/index/btree.h`、`src/index/v2_btree_latch.cpp`",
     "`none` 从 t1 到 t32：YCSB-A 243k→42.8k、YCSB-E 176k→68.3k、TPC-C payment 551k→176k。"
     "没有任何事务锁参与也恶化到这一步，指向 `BTreeLatch` 的 `root_guard_` / `writer_guard_`。",
     "在任何并发结论成立之前，这一层要先解决；否则所有曲线都是两个瓶颈叠加的形状。"),
    ("D", "事务工作 90~99% 被丢弃，且 attempted 吞吐本身也在下降",
     "`src/txn/workload.cpp` 的四个 `run_*` 协议；no-wait 语义",
     "YCSB-A t32：record-2pl attempted 35,388 / committed 3,740；hierarchical attempted 40,498 / committed 421。"
     "attempted 从 t2 的 129k 掉到 t32 的 35k。",
     "这不是「重试太多」，而是每个事务都变慢、持锁时间变长、冲突窗口变大。"
     "需要先拆掉锁表的全局串行点，再谈 no-wait 的代价。"),
    ("E", "TPC-C payment 与 new-order 不可横比",
     "`src/txn/workload.cpp` 的 `payment()` / `new_order()`",
     "payment 只生成 3 个 update，`operations_per_transaction` 对它完全无效；"
     "new-order 才有 `tpcc_items_per_order` 循环。实测 t1 吞吐 551k vs 224k，差 2.5 倍。",
     "同一张图里并列会被读成「payment 更轻」。要么让 payment 也遵守 ops 参数，"
     "要么在报告里明确标注两者事务成本不同构。"),
    ("F", "锁表容量上限被误当成冲突（跨主机）",
     "`include/lock/remote_lock_manager.h` 的 `kRemoteLockSlotsPerShard = 64`",
     "单线程单客户端、`lock_conflicts` 恒为 0 的前提下，中止率仍随「每事务持锁数」单调上升："
     "range 1/4/16/32 → 20/48/159/258 把锁 → 0/0/7/51 次中止。",
     "跨主机那 432 个 case 一个都没跑。要么把槽数提到 512，要么把 capacity 与 conflict 分开计数。"),
    ("G", "矩阵脚本本身的三个缺陷",
     "`scripts/run_formal_matrix.sh`",
     "① CSV 校验比对表头前缀，第一个 case 即中止（已修，未提交）；"
     "② `run_case` 失败即 `return 1`，配合 `set -e` 会让单个 case 超时终止整轮；"
     "③ `experiments/configs/*.json` 未被 runner 读取，配置与命令是两套东西。",
     "②和③仍在。单点超时丢整轮的代价在这一轮已经接近发生（r=128 t8 单例 118 秒）。"),
]


def build():
    total = len(rows)
    ycsb_a = summary_table("ycsb-a")
    ycsb_e = summary_table("ycsb-e")
    pay = summary_table("tpcc-payment")
    newo = summary_table("tpcc-new-order")

    cards = [
        ("756", "case 全部完成", "0 失败，三次重复取中位数"),
        ("2h44m", "总耗时", "02:48 → 05:33，本地矩阵"),
        ("99.0%", "hierarchical t32 中止率", "YCSB-A，committed 仅 421 txn/s"),
        ("60×", "range=128 下与逐行协议的差距", "hierarchical 4,000 vs 约 70 txn/s"),
    ]

    def table_html(title, data, note):
        head = ("<tr><th>协议</th><th>bucket</th><th>t1</th><th>t4</th><th>t32</th>"
                "<th>中止率 @t1</th><th>中止率 @t32</th><th>t1→t32 衰减</th></tr>")
        body = []
        ref = max(d[2] for d in data if d[2]) or 1
        for cc, b, t1, t4, t32, ab1, ab32, drop in data:
            body.append(
                f"<tr><td class=cc>{cc}</td><td>{b or '—'}</td>"
                f"<td class=num>{bar(t1, ref, CC_COLOR[cc])}{fmt(t1)}</td>"
                f"<td class=num>{fmt(t4)}</td><td class=num>{fmt(t32)}</td>"
                f"<td class=num>{ab1:.1f}%</td><td class=num>{ab32:.1f}%</td>"
                f"<td class=num>{drop:.0f}×</td></tr>")
        return (f"<h3>{title}</h3><p class=note>{note}</p>"
                f"<div class=table-wrap><table>{head}{''.join(body)}</table></div>")

    fig_html = []
    for i, (name, title, caption) in enumerate(figures):
        fig_html.append(
            f'<figure><figcaption>{title}</figcaption>'
            f'<img src="{img(name)}" alt="{title}">'
            f'<p class=caption>{caption}</p></figure>')

    prob_html = []
    for code, title, where, evidence, impact in problems:
        prob_html.append(
            f'<div class=problem><div class=ph><span class=code>{code}</span>'
            f'<span class=ptitle>{title}</span></div>'
            f'<p class=where>{where}</p>'
            f'<p><b>证据</b>　{evidence}</p>'
            f'<p><b>影响</b>　{impact}</p></div>')

    html = f"""<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>RdmaRangeLock 正式性能矩阵 · 2026-10-31</title>
<style>
:root {{
  --bg: #ffffff; --fg: #1a1d21; --muted: #5f6b76; --line: #e3e7eb;
  --soft: #f6f8fa; --accent: #0b5fff; --warn: #b42318;
}}
* {{ box-sizing: border-box; }}
body {{
  margin: 0; background: var(--bg); color: var(--fg);
  font: 15px/1.7 -apple-system, BlinkMacSystemFont, "Segoe UI", "Microsoft YaHei", sans-serif;
}}
main {{ max-width: 1080px; margin: 0 auto; padding: 48px 28px 96px; }}
header {{ border-bottom: 2px solid var(--fg); padding-bottom: 20px; margin-bottom: 34px; }}
h1 {{ font-size: 27px; margin: 0 0 10px; letter-spacing: -.3px; }}
.sub {{ color: var(--muted); font-size: 13.5px; }}
.sub code {{ background: var(--soft); padding: 1px 5px; border-radius: 3px; font-size: 12.5px; }}
h2 {{ font-size: 19px; margin: 52px 0 6px; letter-spacing: -.2px; }}
h2 .n {{ color: var(--muted); font-weight: 400; margin-right: 8px; }}
h3 {{ font-size: 15px; margin: 26px 0 4px; }}
p {{ margin: 10px 0; }}
.cards {{ display: grid; grid-template-columns: repeat(4, 1fr); gap: 14px; margin: 26px 0 8px; }}
.card {{ border: 1px solid var(--line); border-radius: 8px; padding: 16px 18px; background: var(--soft); }}
.card .v {{ font-size: 25px; font-weight: 650; letter-spacing: -.5px; }}
.card .l {{ font-size: 13px; font-weight: 600; margin-top: 2px; }}
.card .d {{ font-size: 12px; color: var(--muted); margin-top: 4px; }}
.lead {{ border-left: 3px solid var(--accent); padding: 2px 0 2px 16px; color: #2b3138; margin: 22px 0; }}
ul {{ padding-left: 22px; margin: 10px 0; }}
li {{ margin: 7px 0; }}
figure {{ margin: 30px 0 10px; }}
figcaption {{ font-size: 13.5px; font-weight: 650; margin-bottom: 8px; }}
figure img {{
  width: 100%; height: auto; border: 1px solid var(--line);
  border-radius: 6px; background: #fff;
}}
.caption {{ font-size: 13px; color: var(--muted); margin-top: 8px; }}
.table-wrap {{ overflow-x: auto; margin: 10px 0 4px; }}
table {{ border-collapse: collapse; width: 100%; font-size: 13px; }}
th, td {{ padding: 7px 9px; border-bottom: 1px solid var(--line); text-align: left; white-space: nowrap; }}
th {{ font-weight: 600; color: var(--muted); font-size: 12px; border-bottom: 1.5px solid var(--line); }}
td.num {{ font-variant-numeric: tabular-nums; }}
td.cc {{ font-family: ui-monospace, Consolas, monospace; font-size: 12.5px; }}
.bar {{ display: inline-block; width: 46px; height: 6px; background: #eceff2; border-radius: 3px;
        margin-right: 8px; vertical-align: middle; overflow: hidden; }}
.bar span {{ display: block; height: 100%; border-radius: 3px; }}
.note {{ font-size: 12.5px; color: var(--muted); margin: 2px 0 0; }}
.problem {{ border: 1px solid var(--line); border-radius: 8px; padding: 16px 18px; margin: 14px 0; }}
.problem .ph {{ display: flex; align-items: baseline; gap: 9px; }}
.code {{ display: inline-block; min-width: 22px; text-align: center; background: var(--warn);
         color: #fff; font-size: 12px; font-weight: 700; border-radius: 4px; padding: 1px 6px; }}
.ptitle {{ font-weight: 650; font-size: 14.5px; }}
.problem p {{ font-size: 13.5px; margin: 7px 0; }}
.where {{ color: var(--muted); font-size: 12.5px !important; font-family: ui-monospace, Consolas, monospace; }}
.problem b {{ font-weight: 650; color: #2b3138; }}
footer {{ margin-top: 56px; padding-top: 18px; border-top: 1px solid var(--line);
          font-size: 12.5px; color: var(--muted); }}
@media (max-width: 780px) {{
  .cards {{ grid-template-columns: repeat(2, 1fr); }}
  main {{ padding: 32px 16px 64px; }}
}}
</style>
</head>
<body>
<main>
<header>
  <h1>RdmaRangeLock 正式性能矩阵</h1>
  <div class="sub">
    本地矩阵 · commit <code>b808549</code> · 2026-10-31 02:48–05:33 ·
    控制端 <code>.66</code>（Xeon Gold 5218 ×2, 64 线程, 125 GB）·
    数据源 <code>experiments/results/{os.path.basename(CSV_PATH)}</code><br>sha256 <code>{SHORT_DIGEST}…</code> · {SIZE:,} 字节 · {len(rows)} 个 case
  </div>
</header>

<div class="cards">
  {''.join(f'<div class=card><div class=v>{v}</div><div class=l>{l}</div><div class=d>{d}</div></div>' for v, l, d in cards)}
</div>

<h2><span class=n>01</span>一句话结论</h2>
<p class="lead">
这一轮 756 个 case 全部跑完、零失败，但<b>它测到的不是并发度，而是互斥锁排队</b>。
所有锁协议（以及无锁基线）都随线程数单调下降；唯一干净的对比维度是单线程开销，
以及不同协议在同一范围下付出的锁数量差异。
</p>
<ul>
  <li><b>能用的</b>：单线程吞吐（图 7）、锁数量与每行锁成本（图 5）、bucket 宽度与负载的交互（图 6）。</li>
  <li><b>不能用的</b>：任何"t32 比 t4 快"或"某协议扩展性更好"的说法——全体曲线都在下降，排序只反映谁更早撞上全局互斥锁。</li>
  <li><b>没测到的</b>：幻读防护。YCSB-E 的插入不落在扫描区间内，v3/v4/v5 的中止率全程为 0。</li>
</ul>

<h2><span class=n>02</span>图表</h2>
{''.join(fig_html)}

<h2><span class=n>03</span>完整汇总表</h2>
<p class=note>三次重复取中位数；吞吐单位为 committed txn/s；衰减为 t1 ÷ t32。</p>
{table_html("YCSB-A（range=0）", ycsb_a, "50% 更新、Zipf 0.99 —— 写冲突最重的负载。")}
{table_html("YCSB-E（range=8）", ycsb_e, "范围读为主，插入落在扫描区间之外（见问题 B）。")}
{table_html("TPC-C payment（range=0）", pay, "每事务仅 3 个 update，ops 参数对其无效（见问题 E）。")}
{table_html("TPC-C new-order（range=0）", newo, "含 items 循环，事务成本明显高于 payment。")}

<h2><span class=n>04</span>发现的问题</h2>
<p class=note>A–F 是从这一轮数据里读出来的；G 是执行过程中暴露的脚本缺陷。</p>
{''.join(prob_html)}

<h2><span class=n>05</span>复现</h2>
<ul>
  <li>矩阵：<code>scripts/run_formal_matrix.sh --mode local</code>（tmux 内运行，约 2h45m）</li>
  <li>出图：<code>scripts/plot_formal_matrix.py --mode local</code> → <code>figures/formal/</code></li>
  <li>依赖：matplotlib 3.10.9 + numpy 2.2.6（.66 上通过 <code>pip3 install --user</code> 安装，未进仓库依赖声明）</li>
</ul>

<footer>
  数据：{len(rows)} 个 case，三次重复取中位数。跨主机 RDMA 半张矩阵因锁表容量上限（问题 F）尚未执行，本报告只覆盖本地通路。<br>
  原始数据不入库（<code>.gitignore</code> 忽略 <code>experiments/results/*.csv</code>），归档信息见
  <code>docs/reports/README.md</code>。重新生成：<code>scripts/build_formal_report.py</code><br>
  sha256 <code>{DIGEST}</code>
</footer>
</main>
</body>
</html>
"""
    with io.open(OUT_PATH, "w", encoding="utf-8") as handle:
        handle.write(html)
    print("wrote", OUT_PATH, f"{os.path.getsize(OUT_PATH)/1024:.0f} KB")


if __name__ == "__main__":
    build()
