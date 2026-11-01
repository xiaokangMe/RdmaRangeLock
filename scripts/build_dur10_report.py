#!/usr/bin/env python3
"""Self-contained HTML report for the 10-second-window formal matrices."""

import base64
import glob
import hashlib
import io
import os
import csv_schema
import statistics as st
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RESULTS = os.path.join(ROOT, "experiments", "results")
FIGDIR = os.path.join(ROOT, "figures", "formal-dur10")
OUT = os.path.join(ROOT, "docs", "reports", "formal-matrix-dur10-2026-11-01.html")

THREADS = [1, 2, 4, 8, 16, 32]
LOCAL_CCS = ["none", "record-2pl", "gap-lock", "next-key", "hierarchical"]


def latest(pattern):
    files = sorted(glob.glob(os.path.join(RESULTS, pattern)))
    with open(files[-1], newline="") as h:
        return files[-1], csv_schema.load_rows(h)


def load(name):
    with open(os.path.join(RESULTS, name), newline="") as h:
        return csv_schema.load_rows(h)


def med(rows, f):
    return st.median(float(r[f]) for r in rows)


def idx(rows, key):
    out = defaultdict(list)
    for r in rows:
        out[key(r)].append(r)
    return out


LP, local = latest("formal-local-20261101-*.csv")
CP, cross = latest("formal-cross-host-20261101-*.csv")
old_cross = load("formal-cross-host-20261031-191357.csv")
LSHA = hashlib.sha256(open(LP, "rb").read()).hexdigest()
CSHA = hashlib.sha256(open(CP, "rb").read()).hexdigest()

lg = idx(local, lambda r: (r["workload"], r["cc"], r["bucket"], r["threads"], r["range"]))
cg = idx(cross, lambda r: (r["cc"], r["bucket"], r["threads"], r["workload"], r["range"]))
og = idx(old_cross, lambda r: (r["cc"], r["bucket"], r["threads"], r["workload"], r["range"]))


def img(name):
    with open(os.path.join(FIGDIR, name), "rb") as h:
        return "data:image/png;base64," + base64.b64encode(h.read()).decode()


def local_table(workload, rng, with_abort=True):
    head = "<tr><th>cc</th>" + "".join(f"<th>t{t}</th>" for t in THREADS)
    head += "<th>t1→t32</th><th>中止率 @t32</th></tr>" if with_abort else "</tr>"
    body = []
    for cc in LOCAL_CCS:
        b = "64" if cc == "hierarchical" else "0"
        cells, t1, t32 = [], None, None
        for t in THREADS:
            k = (workload, cc, b, str(t), rng)
            if k not in lg:
                cells.append("—")
                continue
            v = med(lg[k], "throughput_committed_txn_s")
            cells.append(f"{v:,.0f}")
            if t == 1:
                t1 = v
            if t == 32:
                t32 = v
        if with_abort:
            k32 = (workload, cc, b, "32", rng)
            rate = f"{100*med(lg[k32],'aborted')/med(lg[k32],'transactions'):.1f}%" if k32 in lg else "—"
            drop = f"{t1/t32:.0f}×" if t1 and t32 else "—"
            body.append(f"<tr><td class=cc>{cc}</td>" + "".join(f"<td class=num>{c}</td>" for c in cells) +
                        f"<td class=num>{drop}</td><td class=num>{rate}</td></tr>")
        else:
            body.append(f"<tr><td class=cc>{cc}</td>" + "".join(f"<td class=num>{c}</td>" for c in cells) + "</tr>")
    return f"<div class=table-wrap><table>{head}{''.join(body)}</table></div>"


def cross_table():
    series = [("rdma-next-key", "0", "rdma-next-key"),
              ("rdma-hierarchical", "16", "hier b16"),
              ("rdma-hierarchical", "64", "hier b64"),
              ("rdma-hierarchical", "256", "hier b256")]
    head = "<tr><th>cc</th>" + "".join(f"<th>t{t}</th>" for t in THREADS) + "</tr>"
    body = []
    for cc, b, label in series:
        cells = []
        for t in THREADS:
            k = (cc, b, str(t), "ycsb-e", "8")
            if k not in cg:
                cells.append("—")
                continue
            tput = med(cg[k], "throughput_committed_txn_s")
            rate = 100 * med(cg[k], "aborted") / med(cg[k], "transactions")
            cells.append(f"{tput:,.0f}<span class=sub>{rate:.0f}%</span>")
        body.append(f"<tr><td class=cc>{label}</td>" + "".join(f"<td class=num>{c}</td>" for c in cells) + "</tr>")
    return f"<div class=table-wrap><table>{head}{''.join(body)}</table></div>"


def capacity_table():
    head = ("<tr><th>threads</th><th>事务数</th><th>中止</th><th>conflicts</th>"
            "<th>capacity</th><th>capacity 占中止</th><th>提交率</th></tr>")
    body = []
    for t in THREADS:
        k = ("rdma-next-key", "0", str(t), "ycsb-e", "8")
        if k not in cg:
            continue
        rows = cg[k]
        tx, ab = med(rows, "transactions"), med(rows, "aborted")
        cf, cap = med(rows, "lock_conflicts"), med(rows, "lock_capacity_aborts")
        share = f"{100*cap/ab:.0f}%" if ab else "—"
        body.append(f"<tr><td class=num>{t}</td><td class=num>{tx:,.0f}</td><td class=num>{ab:,.0f}</td>"
                    f"<td class=num>{cf:,.0f}</td><td class=num>{cap:,.0f}</td>"
                    f"<td class=num>{share}</td><td class=num>{100*med(rows,'committed')/tx:.1f}%</td></tr>")
    return f"<div class=table-wrap><table>{head}{''.join(body)}</table></div>"


def old_new_table():
    head = ("<tr><th>cc</th><th>threads</th><th>旧 5000 事务窗口</th>"
            "<th>新 10 秒窗口</th><th>变化</th></tr>")
    body = []
    for cc, b, label in [("rdma-next-key", "0", "rdma-next-key"),
                         ("rdma-hierarchical", "64", "hier b64")]:
        for t in THREADS:
            k = (cc, b, str(t), "ycsb-e", "8")
            if k not in cg or k not in og:
                continue
            new = med(cg[k], "throughput_committed_txn_s")
            old = med(og[k], "throughput_committed_txn_s")
            body.append(f"<tr><td class=cc>{label}</td><td class=num>{t}</td>"
                        f"<td class=num>{old:,.0f}</td><td class=num>{new:,.0f}</td>"
                        f"<td class=num>{100*(new-old)/old:+.0f}%</td></tr>")
    return f"<div class=table-wrap><table>{head}{''.join(body)}</table></div>"


FIGS = [
    ("fig13_window_quality.png", "图 1　窗口质量",
     "1188 次运行全部 ≥10 秒。本地最长 45 秒（gap-lock / range=128 / t32），"
     "因为截止时间只在事务之间检查，单个长事务不会被中断——对吞吐无害，但让总时长不可预测。"),
    ("fig14_local_scaling_dur10.png", "图 2　本地扩展性",
     "无锁基线 334k → 196k（1.7×），锁协议 41~54×。仅 30% 的中止解释不了 43 倍衰减，"
     "剩余串行点在锁表内部。"),
    ("fig15_local_abort_dur10.png", "图 3　本地中止率",
     "四个协议都在 30% 附近饱和，hierarchical 高 6~7 个百分点（桶锁粒度更粗）。"
     "这是真实的范围冲突，不是上一轮那种 1−1/T 的人为形态。"),
    ("fig16_cross_abort_dur10.png", "图 4　跨主机中止率（与旧形态对比）",
     "虚线是上一轮的 1−1/threads。新代码的中止率随负载变化，不再被这条虚线钉住，"
     "说明 gate 泄漏确实修好了。rdma-next-key 在 t32 冲到 87%，其中大部分是容量中止。"),
    ("fig17_cross_abort_composition.png", "图 5　rdma-next-key 的中止构成",
     "t16 起出现 capacity 中止，t32 时 83% 的中止来自容量而不是冲突。"
     "32 并发 × 每事务约 130 把锁 ≈ 4,160，已超过 16×256 = 4,096 的总槽数。"),
    ("fig18_old_vs_new_window.png", "图 6　旧窗口 vs 新窗口",
     "旧曲线“看似平坦”，其实是事务瞬间中止、elapsed 只有 0.1~0.26 秒导致的假象；"
     "新窗口下同一配置是 t4 峰值、之后单调下降。"),
]

PROBLEMS = [
    ("P1", "锁槽容量重新成为限制（跨主机）",
     "lock_capacity_aborts 合计 142,025；34/432 次运行命中；集中在 rdma-next-key t≥16",
     "gate 修好后事务能活得更久，攒下的锁反而把分片槽位打满。t32 / range=8 时 7,408 次中止里有 "
     "6,158 次是容量（83%），提交率掉到 13%。256 槽/片对“range≥8 × 并发≥16”不够。"),
    ("P2", "rdma-next-key 峰值在 t4，之后单调下降",
     "t1 511 → t4 839 → t8 477 → t16 277 → t32 95",
     "容量中止与真实冲突叠加：t32 时事务还没做完就被踢出。传输层不是唯一瓶颈，"
     "但容量把并发的收益吃掉了。"),
    ("P3", "hierarchical 中止率随 bucket 增大，吞吐却几乎不变",
     "t32：b16 33.0%/1,034 · b64 36.7%/1,065 · b256 39.6%/1,069",
     "中止率涨 6.6 个点、吞吐不动，说明这个点上限制已经不是锁粒度，而是每次加锁的 RDMA 往返。"),
    ("P4", "本地锁协议仍衰减 41~54 倍，远超过中止率能解释的部分",
     "record-2pl 53.5k→1.2k（43×），中止率仅 29.8%；同窗口 none 只掉 1.7×",
     "unlock_all 的问题修掉后仍在崩，剩余串行点在锁表内部——怀疑 lock() 对分片内 "
     "vector<RangeLock> 的线性扫描。需要用计数器验证，暂不作结论。"),
    ("P5", "本地 hierarchical 在两个负载上排序完全相反",
     "YCSB-E 164k→10.8k（15×）；YCSB-A 231k→408（567×）",
     "根意向锁用 unbounded() 区间，shard_ids 对它返回全部分片，v6 的每把根锁都是全表锁。"
     "写 v6 那一节必须两条曲线成对出现。"),
    ("P6", "窗口 overshoot：deadline 只在事务之间检查",
     "本地 18 例 >12 秒，最长 45 秒（gap-lock / range=128 / t32）",
     "慢配置的实际窗口远大于 10 秒。对吞吐计算无害，但让总时长不可预测（本轮比估算多 30 分钟）。"),
    ("P7", "旧窗口的跨主机曲线不可复用",
     "旧：t1–t32 几乎持平 511→555；新：t4 峰值 839、t32 95",
     "旧曲线平坦是因为一半事务瞬间中止、elapsed 只有 0.1~0.26 秒。凡是基于 5000 事务窗口的"
     "跨主机吞吐数字都不要引用。"),
]


def build():
    el_local = sorted(float(r["elapsed_ns"]) / 1e9 for r in local)
    el_cross = sorted(float(r["elapsed_ns"]) / 1e9 for r in cross)
    to_local = sum(float(r["rdma_timeouts"]) for r in local)
    to_cross = sum(float(r["rdma_timeouts"]) for r in cross)
    cap_cross = sum(float(r["lock_capacity_aborts"]) for r in cross)
    gate_cross = sum(float(r["lock_gate_aborts"]) for r in cross)
    tx_cross = sum(float(r["transactions"]) for r in cross)
    ab_cross = sum(float(r["aborted"]) for r in cross)

    cards = [
        ("1,188", "次运行全部完成", "本地 756 · 跨主机 432 · 0 失败"),
        ("10.00 s", "统一测量窗口", f"最短 {min(el_local+el_cross):.2f}s · 无短窗口"),
        ("0", "RDMA 超时", "上一轮 36/432 次运行被 1 秒卡顿污染"),
        ("22.3%", "跨主机中止率", f"其中 2.5% 来自容量 · gate 归零"),
    ]
    figs = "".join(
        f"<figure><figcaption>{t}</figcaption><img src=\"{img(n)}\" alt=\"{t}\">"
        f"<p class=caption>{c}</p></figure>" for n, t, c in FIGS)
    probs = "".join(
        f"<div class=problem><div class=ph><span class=code>{c}</span>"
        f"<span class=ptitle>{t}</span></div><p class=where>{e}</p><p>{b}</p></div>"
        for c, t, e, b in PROBLEMS)

    html = f"""<!DOCTYPE html>
<html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>RdmaRangeLock 10 秒窗口正式矩阵 · 2026-11-01</title>
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
h3 {{ font-size:14px; margin:26px 0 2px; }}
p {{ margin:10px 0; }}
.note {{ font-size:12.5px; color:var(--muted); }}
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
.sub2 {{ }} .sub {{ }}
td .sub {{ font-size:10px; color:var(--muted); margin-left:5px; }}
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
  <h1>RdmaRangeLock 正式矩阵：10 秒测量窗口</h1>
  <div class="sub">
    commit <code>b20aa39</code>（含 <code>12b5890</code> 的锁表观测与多 QP 修复）·
    2026-11-01 03:00–06:28 UTC ·
    控制端 <code>.66</code>（Xeon Gold 5218 ×2, 64 线程）· RDMA 客户端 <code>.67</code><br>
    <code>{os.path.basename(LP)}</code>（{len(local)} runs）+
    <code>{os.path.basename(CP)}</code>（{len(cross)} runs）
  </div>
</header>

<div class="cards">
{''.join(f'<div class=card><div class=v>{v}</div><div class=l>{l}</div><div class=d>{d}</div></div>' for v, l, d in cards)}
</div>

<h2><span class=n>01</span>这轮改变了什么</h2>
<p class="lead">
上一轮的单 case 只有 5000 个事务、正常耗时 0.14~0.26 秒，一次 1 秒 RDMA 超时就能把吞吐压掉
5~20 倍，36/432 次运行被污染。本轮把测量窗口改成<b>固定时长 10 秒</b>：
同样的卡顿最多影响 10%，<b>一行数据都不用剔除</b>。
</p>
<ul>
  <li><b>窗口</b>：本地 {len(local)} 次全部 10.00 秒起，跨主机 {len(cross)} 次 10.00–{max(el_cross):.2f} 秒，无短窗口。</li>
  <li><b>超时</b>：`rdma_timeouts` 与 `rdma_completion_errors` 合计为 <b>0</b>（本地 {to_local:.0f}、跨主机 {to_cross:.0f}）。</li>
  <li><b>gate 泄漏</b>：`lock_gate_aborts` 从上轮 1,088,370 降到 <b>{gate_cross:.0f}</b>，中止率不再等于 1−1/threads。</li>
  <li><b>新问题</b>：容量中止回来了，{cap_cross:,.0f} 次（占中止 2.5%），集中在 rdma-next-key 的 t≥16。</li>
</ul>

<h2><span class=n>02</span>图表</h2>
{figs}

<h2><span class=n>03</span>本地汇总</h2>
<p class=note>三次重复取中位数，单位 committed txn/s；末两列为 t1→t32 衰减与 t32 中止率。</p>
<h3>YCSB-E（range=8）</h3>
{local_table("ycsb-e", "8")}
<h3>YCSB-A（range=0）</h3>
{local_table("ycsb-a", "0")}

<h2><span class=n>04</span>跨主机汇总</h2>
<p class=note>YCSB-E range=8，格式为「committed txn/s（中止率）」。</p>
{cross_table()}
<h3>rdma-next-key 的中止构成</h3>
<p class=note>容量中止从 t16 开始出现，t32 时占该点中止的 83%。</p>
{capacity_table()}
<h3>同一配置：旧窗口 vs 新窗口</h3>
{old_new_table()}

<h2><span class=n>05</span>发现的问题</h2>
{probs}

<h2><span class=n>06</span>复现</h2>
<ul>
  <li>运行：<code>RRL_DURATION=10 scripts/run_formal_matrix.sh --mode local|cross-host</code></li>
  <li>汇总：<code>scripts/analyze_dur10.py</code>；出图：<code>scripts/plot_dur10.py</code></li>
  <li>判读：<code>elapsed_ns ≥ 1e10</code>；容量类中止看 <code>lock_capacity_aborts</code>，不要看总 <code>aborted</code></li>
</ul>

<footer>
原始数据按 CONSTRAINTS 第四条不入库，校验和：<br>
local <code>{LSHA}</code><br>
cross <code>{CSHA}</code>
</footer>
</main></body></html>
"""
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with io.open(OUT, "w", encoding="utf-8") as h:
        h.write(html)
    print("wrote", OUT, f"{os.path.getsize(OUT)/1024:.0f} KB")


if __name__ == "__main__":
    build()
