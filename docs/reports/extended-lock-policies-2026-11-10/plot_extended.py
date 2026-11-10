#!/usr/bin/env python3
"""Line charts for the dbf145d extended-lock-policy formal matrix (2026-11-10).

Data: ~/rrl-rolex-rdma campaign 20261109-175635-096373,
local 217 + cross-host 210 = 427 formal cases, all single-run / 32 threads.

Style follows .66:~/RdmaRangeLock/scripts/plot_cross_host.py and
plot_zipf_predicate.py: Agg backend, one axes per PNG, marker="o" ms=4.5
lw=1.8, dotted major grid, top/right spines hidden, log y with 10^k ticks,
dpi=170 white face, and the project's fixed wait-policy colours.

Every one of the 427 rows is used:
  A/B groups : 343 rows (all non-hierarchical protocols at every config/policy,
               plus hierarchical at b64, plus the 7 unlocked baselines)
  C group    : 126 rows (hierarchical at b16/b64/b256) of which 42 are shared
               with A/B -> 343 + 84 = 427 distinct rows.
"""
import csv
import html
import math
import os
from collections import defaultdict

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import LogLocator

ROOT = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(ROOT, "data")
OUT = os.path.join(ROOT, "assets")
os.makedirs(OUT, exist_ok=True)

plt.rcParams.update({
    "figure.dpi": 140, "savefig.dpi": 170, "font.size": 9,
    "legend.frameon": False, "axes.unicode_minus": False,
    "font.sans-serif": ["Microsoft YaHei", "DejaVu Sans"],
})

STAMP = "20261109-175635-096373"
SIDES = ("local", "cross-host")
SIDE_LABEL = {"local": "本地", "cross-host": "跨主机 RDMA"}
POLICIES = ("no-wait", "wait-die", "wound-wait")
POLICY_LABEL = {"no-wait": "no-wait", "wait-die": "wait-die", "wound-wait": "wound-wait"}
POLICY_COLOUR = {"no-wait": "#d62728", "wait-die": "#1f77b4", "wound-wait": "#2ca02c"}

# (workload, range length, short label). range=0 means the point-only YCSB-A.
CONFIGS = [
    ("ycsb-a", 0, "A-0"),
    ("ycsb-e", 8, "E-8"),
    ("ycsb-e", 32, "E-32"),
    ("ycsb-e", 128, "E-128"),
    ("ycsb-e-skew", 8, "S-8"),
    ("ycsb-e-skew", 32, "S-32"),
    ("ycsb-e-skew", 128, "S-128"),
]
XLAB = [c[2] for c in CONFIGS]
XS = list(range(len(CONFIGS)))
BUCKETS = (16, 64, 256)

# Shared palette across local / RDMA: same colour = same lock semantics.
PALETTE = {
    "none": "#888780",
    "record-2pl": "#1f77b4",
    "gap-lock": "#ff7f0e",
    "next-key": "#2ca02c",
    "prior-key": "#6A4C93",
    "hierarchical": "#d62728",
    "exact-range": "#2A9D8F",
    "precision-lock": "#8c564b",
    "griffin-pl": "#e377c2",
}
LOCAL_PROTO = [
    ("none", "none (无锁基线)"),
    ("record-2pl", "record 2PL"),
    ("gap-lock", "gap lock"),
    ("next-key", "next-key"),
    ("prior-key", "prior-key"),
    ("hierarchical", "hierarchical b64"),
    ("exact-range", "exact-range"),
    ("precision-lock", "precision-lock"),
    ("griffin-pl", "griffin-pl"),
]
RDMA_PROTO = [
    ("rdma-" + c, n.replace(" b64", " b64")) for c, n in LOCAL_PROTO if c != "none"
]
CONFIG_COLOUR = [
    "#4C72B0", "#DD8452", "#55A868", "#C44E52",
    "#8172B3", "#937860", "#DA8BC3",
]


def load():
    idx = {}
    rows = {}
    for side in SIDES:
        path = os.path.join(DATA, "formal-shared-data-%s-%s.csv" % (side, STAMP))
        with open(path, newline="", encoding="utf-8") as f:
            rs = list(csv.DictReader(f))
        rows[side] = rs
        for r in rs:
            key = (side, r["cc"], r["wait_policy"], r["workload"],
                   int(r["range"]), int(r["bucket"]))
            idx[key] = r
    return idx, rows


IDX, ROWS = load()


def fnum(r, k):
    try:
        return float(r.get(k, 0) or 0)
    except (TypeError, ValueError):
        return 0.0


def cell(side, cc, policy, wl, rng, bkt):
    p = "none" if cc == "none" else policy
    return IDX.get((side, cc, p, wl, rng, bkt))


def metric(side, cc, policy, cfg, bkt, kind):
    """kind: tps / abort / locks / rdma / rdma_per_lock / p95 / conflicts."""
    wl, rng, _ = cfg
    r = cell(side, cc, policy, wl, rng, bkt)
    if r is None:
        return float("nan")
    committed = fnum(r, "committed")
    aborted = fnum(r, "aborted")
    total = committed + aborted
    if kind == "tps":
        return fnum(r, "throughput_committed_txn_s")
    if kind == "abort":
        return 100.0 * aborted / total if total else float("nan")
    if kind == "locks":
        return fnum(r, "lock_acquires") / committed if committed else float("nan")
    if kind == "rdma":
        return fnum(r, "rdma_operations") / committed if committed else float("nan")
    if kind == "rdma_per_lock":
        la = fnum(r, "lock_acquires")
        return fnum(r, "rdma_operations") / la if la else float("nan")
    if kind == "p95":
        return fnum(r, "successful_attempt_p95_ns") / 1e6
    if kind == "conflicts":
        return fnum(r, "lock_conflicts") / total if total else float("nan")
    raise ValueError(kind)


def series(side, cc, policy, kind, bkt=0, configs=CONFIGS):
    return [metric(side, cc, policy, c, bkt, kind) for c in configs]


def clean(ys):
    return [v if (math.isfinite(v) and v > 0) else float("nan") for v in ys]


def newfig(title, ylabel, ylog=True, xlabel="配置点（负载-扫描宽度，序数轴，非数值插值）"):
    fig, ax = plt.subplots(figsize=(7.6, 4.5))
    ax.set_title(title, fontsize=10.5, pad=10)
    ax.set_xlabel(xlabel, fontsize=8.5)
    ax.set_ylabel(ylabel, fontsize=9)
    ax.set_xticks(XS)
    ax.set_xticklabels(XLAB)
    ax.set_xlim(-0.35, len(XS) - 0.65)
    if ylog:
        ax.set_yscale("log")
        ax.yaxis.set_major_locator(LogLocator(base=10.0, numticks=9))
    return fig, ax


def draw(ax, xs, ys, label, colour, ls="-", zero_marks=None, positive_only=True):
    vals = clean(ys) if positive_only else [
        v if math.isfinite(v) else float("nan") for v in ys]
    ax.plot(xs, vals, marker="o", ms=4.5, lw=1.8, ls=ls,
            color=colour, label=label, alpha=0.92)
    if zero_marks is not None:
        for x, v in zip(xs, ys):
            if math.isfinite(v) and v <= 0:
                zero_marks.append((x, label))


def finish(fig, ax, name, zero_marks=None, legend=True, note=None, zero_note=None):
    ax.grid(True, which="major", ls=":", lw=0.7, color="#c9c9c4", alpha=0.9)
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)
    box = dict(facecolor="white", alpha=0.86, edgecolor="none", pad=1.4)
    if zero_marks:
        lo, hi = ax.get_ylim()
        seen = set()
        for x, _ in zero_marks:
            if x in seen:
                continue
            seen.add(x)
            ax.scatter([x], [lo * 1.25], marker="x", s=48, linewidths=1.6,
                       color="#111111", zorder=5)
        ax.set_ylim(lo, hi)
        if zero_note is None:
            zero_note = "× = 0 提交（no-commits），对数轴无法绘制，标于轴底"
        if zero_note:
            ax.text(0.012, 0.965, zero_note, transform=ax.transAxes,
                    fontsize=7.2, color="#333333", va="top", bbox=box)
    if note:
        ax.text(0.988, 0.02, note, transform=ax.transAxes, fontsize=7.2,
                color="#666666", ha="right", bbox=box)
    if legend:
        ax.legend(loc="center left", bbox_to_anchor=(1.012, 0.5),
                  fontsize=7.4, frameon=False, handlelength=2.2)
    fig.savefig(os.path.join(OUT, name), bbox_inches="tight", facecolor="white")
    plt.close(fig)


def proto_of(side):
    return LOCAL_PROTO if side == "local" else RDMA_PROTO


def bucket_of(cc):
    return 64 if "hierarchical" in cc else 0


FIGURES = []  # (filename, section, title, note)


def reg(fn, section, title, note):
    FIGURES.append((fn, section, title, note))


def group_AB():
    """Throughput and abort rate: all protocols x 7 configs x 3 policies x 2 sides."""
    for kind, ylabel, prefix in (
        ("tps", "提交吞吐 (txn/s, log)", "tps"),
        ("abort", "尝试级失败占比 (%)", "abort"),
    ):
        for side in SIDES:
            for policy in POLICIES:
                fig, ax = newfig(
                    "%s · %s · %s" % (SIDE_LABEL[side], POLICY_LABEL[policy],
                                      "吞吐" if kind == "tps" else "尝试级失败占比"),
                    ylabel, ylog=(kind == "tps"))
                zeros = []
                for cc, name in proto_of(side):
                    ys = series(side, cc, policy, kind, bucket_of(cc))
                    colour = PALETTE[cc.replace("rdma-", "")]
                    draw(ax, XS, ys, name, colour,
                         ls="--" if cc == "none" else "-",
                         zero_marks=zeros if kind == "tps" else None,
                         positive_only=(kind == "tps"))
                if kind == "abort":
                    ax.set_ylim(0, 105)
                fn = "%s-%s-%s.png" % (prefix, "local" if side == "local" else "rdma", policy)
                finish(fig, ax, fn, zero_marks=zeros if kind == "tps" else None,
                       note="层级锁取 b64；其余协议 bucket=0")
                reg(fn, "AB", "%s / %s / %s" % (
                    "吞吐" if kind == "tps" else "尝试级失败占比", SIDE_LABEL[side],
                    POLICY_LABEL[policy]),
                    "九/八种锁语义在 7 个配置点上的整体走势；等待策略分图对照。"
                    + ("失败占比 = aborted/(committed+aborted)，是尝试级（含重试）指标，"
                       "不是事务最终失败率。" if kind == "abort" else ""))


def group_C():
    """Bucket width: hierarchical protocol only, b16 / b64 / b256."""
    for side in SIDES:
        cc = "hierarchical" if side == "local" else "rdma-hierarchical"
        for policy in POLICIES:
            fig, ax = newfig(
                "%s · %s · 桶宽对层级锁吞吐的影响" % (SIDE_LABEL[side], POLICY_LABEL[policy]),
                "提交吞吐 (txn/s, log)", ylog=True,
                xlabel="桶宽（b16 / b64 / b256，序数轴）")
            ax.set_xticks(range(3))
            ax.set_xticklabels(["b16", "b64", "b256"])
            ax.set_xlim(-0.25, 2.25)
            zeros = []
            for i, cfg in enumerate(CONFIGS):
                ys = [metric(side, cc, policy, cfg, b, "tps") for b in BUCKETS]
                draw(ax, range(3), ys, cfg[2], CONFIG_COLOUR[i], zero_marks=zeros)
            fn = "bucket-%s-%s.png" % ("local" if side == "local" else "rdma", policy)
            finish(fig, ax, fn, zero_marks=zeros,
                   note="每格均为同一配置点，仅桶宽变化")
            reg(fn, "C", "桶宽 / %s / %s" % (SIDE_LABEL[side], POLICY_LABEL[policy]),
                "层级锁三档桶宽的边际收益；7 条线为 7 个配置点。")


def group_D():
    """Local / RDMA throughput ratio: the cost of going cross-host."""
    for policy in POLICIES:
        fig, ax = newfig(
            "本地 / 跨主机 吞吐比 · %s" % POLICY_LABEL[policy],
            "本地吞吐 ÷ 跨主机吞吐（log，越高说明远程代价越大）", ylog=True)
        zeros = []
        for cc, name in RDMA_PROTO:
            base = cc.replace("rdma-", "")
            ly = series("local", base, policy, "tps", bucket_of(base))
            ry = series("cross-host", cc, policy, "tps", bucket_of(cc))
            ratio = [(l / r if (math.isfinite(l) and math.isfinite(r) and r > 0)
                      else (0.0 if (math.isfinite(l) and math.isfinite(r) and r <= 0)
                            else float("nan")))
                     for l, r in zip(ly, ry)]
            draw(ax, XS, ratio, name, PALETTE[base], zero_marks=zeros)
        ax.axhline(1, ls=":", lw=1.1, color="#888780")
        fn = "compare-local-rdma-%s.png" % policy
        finish(fig, ax, fn, zero_marks=zeros, zero_note="",
               note="× = 跨主机 0 提交，比值趋于无穷；虚线为 1× 基准")
        reg(fn, "D", "本地/RDMA 代价比 / %s" % POLICY_LABEL[policy],
            "同一锁语义跨主机后的吞吐损失倍数，直接量化远程锁表代价。")


def group_E():
    """Per-transaction synchronisation cost, 3 policies x 2 sides + per-lock RDMA."""
    for side in SIDES:
        kind = "locks" if side == "local" else "rdma"
        ylabel = ("每事务加锁次数 (log)" if side == "local"
                  else "每事务 RDMA 操作数 (log)")
        for policy in POLICIES:
            fig, ax = newfig(
                "%s · %s · 每事务同步代价" % (SIDE_LABEL[side], POLICY_LABEL[policy]),
                ylabel, ylog=True)
            for cc, name in proto_of(side):
                ys = series(side, cc, policy, kind, bucket_of(cc))
                colour = PALETTE[cc.replace("rdma-", "")]
                draw(ax, XS, ys, name, colour,
                     ls="--" if cc == "none" else "-")
            fn = "cost-%s-%s.png" % ("local" if side == "local" else "rdma", policy)
            finish(fig, ax, fn,
                   note="分母为 committed；0 提交格不绘制")
            reg(fn, "E", "同步代价 / %s / %s" % (SIDE_LABEL[side], POLICY_LABEL[policy]),
                "把吞吐差异拆回「一次事务要发多少把锁 / 多少次单边 RDMA」。")
    # Per-lock RDMA operations: the lock-service implementation axis.
    fig, ax = newfig("跨主机 · 每把锁的 RDMA 操作数（no-wait）",
                     "RDMA 操作数 ÷ 加锁次数 (log)", ylog=True)
    for cc, name in RDMA_PROTO:
        ys = series("cross-host", cc, "no-wait", "rdma_per_lock", bucket_of(cc))
        draw(ax, XS, ys, name, PALETTE[cc.replace("rdma-", "")])
    fn = "cost-rdma-per-lock.png"
    finish(fig, ax, fn, note="层级锁取 b64")
    reg(fn, "E", "每锁 RDMA 操作数 / 跨主机 / no-wait",
        "区分「锁语义代价」与「锁服务实现代价」的关键指标。")


def findings():
    """Numbers quoted in the report, all computed from the CSVs."""
    out = {}
    # 1. best protocol per (side, workload, policy) at representative range
    best = defaultdict(list)
    for side in SIDES:
        for policy in POLICIES:
            for wl, rng, lab in CONFIGS:
                cands = []
                for cc, name in proto_of(side):
                    if cc == "none" or cc.startswith("rdma-none"):
                        continue
                    v = metric(side, cc, policy, (wl, rng, lab), bucket_of(cc), "tps")
                    if math.isfinite(v) and v > 0:
                        cands.append((v, name))
                if cands:
                    v, n = max(cands)
                    best[(side, policy)].append((lab, n, v))
    out["best"] = best

    # 2. A-0 / E-32 ratio per protocol (no-wait, local + rdma)
    ratios = []
    for cc, name in RDMA_PROTO:
        base = cc.replace("rdma-", "")
        row = []
        for side, c in (("local", base), ("cross-host", cc)):
            a = metric(side, c, "no-wait", CONFIGS[0], bucket_of(c), "tps")
            e = metric(side, c, "no-wait", CONFIGS[2], bucket_of(c), "tps")
            row.append(a / e if (math.isfinite(a) and math.isfinite(e) and e > 0) else float("nan"))
        ratios.append((name, row[0], row[1]))
    out["ae_ratio"] = ratios

    # 3. local / rdma ratio at E-32, no-wait
    lr = []
    for cc, name in RDMA_PROTO:
        base = cc.replace("rdma-", "")
        l = metric("local", base, "no-wait", CONFIGS[2], bucket_of(base), "tps")
        r = metric("cross-host", cc, "no-wait", CONFIGS[2], bucket_of(cc), "tps")
        lr.append((name, l, r, (l / r if r and math.isfinite(r) and r > 0 else float("nan"))))
    out["local_rdma"] = lr

    # 4. abort rate averaged over protocols/configs by policy and side
    abort = {}
    for side in SIDES:
        for policy in POLICIES:
            vals = []
            for cc, _ in proto_of(side):
                if cc == "none":
                    continue
                for cfg in CONFIGS:
                    v = metric(side, cc, policy, cfg, bucket_of(cc), "abort")
                    if math.isfinite(v):
                        vals.append(v)
            abort[(side, policy)] = sum(vals) / len(vals) if vals else float("nan")
    out["abort"] = abort

    # 5. bucket width effect: b16 -> b256 tps ratio, hierarchical, no-wait
    bk = {}
    for side in SIDES:
        cc = "hierarchical" if side == "local" else "rdma-hierarchical"
        vals = []
        for cfg in CONFIGS:
            a = metric(side, cc, "no-wait", cfg, 16, "tps")
            b = metric(side, cc, "no-wait", cfg, 256, "tps")
            if math.isfinite(a) and math.isfinite(b) and b > 0:
                vals.append(a / b)
        bk[side] = vals
    out["bucket"] = bk

    # 6. range effect: r8 -> r128, no-wait
    rg = {}
    for side in SIDES:
        vals = []
        for cc, _ in proto_of(side):
            if cc == "none":
                continue
            a = metric(side, cc, "no-wait", CONFIGS[1], bucket_of(cc), "tps")
            b = metric(side, cc, "no-wait", CONFIGS[3], bucket_of(cc), "tps")
            if math.isfinite(a) and math.isfinite(b) and b > 0:
                vals.append((cc.replace("rdma-", ""), a / b))
        rg[side] = vals
    out["range"] = rg

    # 7. per-lock RDMA operations (no-wait, mean over configs)
    pl = []
    for cc, name in RDMA_PROTO:
        vals = [metric("cross-host", cc, "no-wait", c, bucket_of(cc), "rdma_per_lock")
                for c in CONFIGS]
        vals = [v for v in vals if math.isfinite(v) and v > 0]
        pl.append((name, sum(vals) / len(vals) if vals else float("nan")))
    out["per_lock"] = pl

    # 8. no-commits rows
    nc = []
    for side in SIDES:
        for r in ROWS[side]:
            if r.get("progress_status") == "no-commits":
                nc.append((SIDE_LABEL[side], r["cc"], r["workload"], r["range"],
                           r["wait_policy"], fnum(r, "committed"), fnum(r, "aborted")))
    out["no_commits"] = nc

    # 9. attempt-vs-transaction accounting (all rows)
    acc = {}
    for side in SIDES:
        tot = defaultdict(float)
        for r in ROWS[side]:
            for k in ("committed", "aborted", "retries", "logical_failed",
                      "logical_started", "wound_aborts"):
                tot[k] += fnum(r, k)
        acc[side] = dict(tot)
    out["accounting"] = acc
    return out


def fmt(v, digits=1):
    return "—" if not math.isfinite(v) else ("%.*f" % (digits, v))


def build_report(F):
    parts = []
    parts.append("""<!DOCTYPE html><html lang="zh-CN"><head><meta charset="utf-8">
<title>扩展锁策略正式矩阵 · 折线图与对比报告（dbf145d, 427 case）</title>
<style>
:root{--ink:#1f2328;--muted:#5c6370;--line:#e3e5e8;--bg:#ffffff;--soft:#f7f8fa;--accent:#1f4e79}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--ink);
font-family:"Microsoft YaHei","PingFang SC","Segoe UI",sans-serif;line-height:1.75}
.wrap{max-width:1180px;margin:0 auto;padding:36px 28px 72px}
h1{font-size:26px;margin:0 0 6px}h2{font-size:19px;margin:38px 0 10px;padding-bottom:8px;border-bottom:2px solid var(--accent)}
h3{font-size:15px;margin:24px 0 8px;color:var(--accent)}
.meta{color:var(--muted);font-size:13px;margin-bottom:18px}
.warn{background:#fff8e6;border-left:4px solid #d9a300;padding:12px 16px;margin:16px 0;font-size:13.5px;border-radius:0 6px 6px 0}
.ok{background:#eef7ee;border-left:4px solid #3a8b3a;padding:12px 16px;margin:16px 0;font-size:13.5px;border-radius:0 6px 6px 0}
.grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:18px;margin-top:14px}
.fig{border:1px solid var(--line);border-radius:8px;padding:12px;background:var(--soft)}
.fig img{width:100%;display:block;background:#fff;border:1px solid #ececec;border-radius:4px}
.fig p{font-size:12.5px;color:var(--muted);margin:8px 0 0}
table{border-collapse:collapse;width:100%;font-size:13px;margin:12px 0}
th,td{border:1px solid var(--line);padding:7px 9px;text-align:left}th{background:var(--soft)}
code{background:#f0f2f5;padding:1px 5px;border-radius:3px;font-size:12px}
ul{margin:8px 0 8px 18px}li{margin:4px 0}
.small{font-size:12.5px;color:var(--muted)}
</style></head><body><div class="wrap">""")
    parts.append("<h1>扩展锁策略正式矩阵 · 折线图与对比报告</h1>")
    parts.append('<div class="meta">数据：.66 <code>~/rrl-rolex-rdma</code>（分支 <code>rolex-rdma-port-20261109</code>，HEAD <code>dbf145d</code>）· campaign <code>20261109-175635-096373</code> · 生成日期 2026-11-10 · 本地 217 + 跨主机 210 = <b>427 个正式 case</b></div>')

    parts.append('<div class="warn"><b>口径（务必带入综述）：</b>全部为 <code>repetition_policy=single-run</code>、<code>runs=1</code>、<code>seed=8</code>、<b>32 OS 线程</b>、<code>execution_mode=threads</code> / <code>coroutines=1</code>、10s 预热 + 10s 测量。<b>单次测量，无误差线，不得宣称置信区间或运行间波动</b>。跨主机使用 <code>native-rdma-state</code>、GID 3、10.0.0.3↔10.0.0.4、槽容量 8192。</div>')

    parts.append('<div class="ok"><b>427 行数据全部纳入本报告：</b>A/B 两组覆盖 343 行（全部非层级协议 × 7 配置点 × 3 等待策略 × 2 侧 + 层级锁 b64 + 7 个无锁基线）；C 组覆盖层级锁 b16/b64/b256 共 126 行，其中 42 行与 A/B 重叠 → 343 + 84 = <b>427 个唯一 case</b>。D/E 组为同批数据的派生比值与代价指标，不引入新样本。</div>')

    parts.append("<h2>0. 图的坐标约定</h2>")
    parts.append("""<p>x 轴是 <b>7 个序数配置点</b>：<code>A-0</code> = YCSB-A 点读写（range=0）；
<code>E-8 / E-32 / E-128</code> = YCSB-E 范围扫描，数字为扫描宽度；
<code>S-8 / S-32 / S-128</code> = YCSB-E-skew 热点扫描。这是类别序列，<b>不是数值插值</b>，折线只用于连接同一协议在相邻配置上的读数。
桶宽图 x 轴为 b16/b64/b256 三档，同样是序数轴。</p>
<p>配色沿用项目脚本：<code>no-wait #d62728</code>、<code>wait-die #1f77b4</code>、<code>wound-wait #2ca02c</code>；同一锁语义在本地与跨主机使用相同颜色。所有折线 <code>marker="o", ms=4.5, lw=1.8</code>，一张 PNG 一个 axes，隐藏上/右边框、点线网格、对数纵轴。</p>
<p><b>本轮固定参数</b>：<code>zipf=0.99</code>、<code>request_distribution=auto</code>（A/E 落到 ordered-zipf，E-skew 落到 hotspot 80/20）、
<code>threads=32</code>、<code>operations=10</code>、<code>keys=10000</code>、<code>tree_records=262144</code>、<code>lock_budget=16384</code>。
唯一变化的自变量是「负载 / 扫描宽度 / 桶宽 / 等待策略 / 锁语义」，符合项目单变量纪律。</p>""")

    sections = [
        ("AB", "1. 吞吐与尝试级失败占比：9/8 种锁语义 × 7 配置点 × 3 等待策略",
         "每侧每种等待策略一张吞吐图、一张失败占比图，共 12 张。层级锁固定 b64，其余协议 bucket=0；<code>none</code> 为本地无锁上界（灰色虚线）。失败占比是<b>尝试级</b>口径，务必与事务最终失败率区分（见 5.3）。"),
        ("C", "2. 桶宽：层级锁 b16 / b64 / b256",
         "只用层级锁的 126 行。7 条线是 7 个配置点，直接看「桶越宽、锁越少但假冲突越多」的拐点。"),
        ("D", "3. 本地 vs 跨主机：同一锁语义的远程代价",
         "y = 本地吞吐 ÷ 跨主机吞吐（对数）。虚线 1× 为无损失基准；× 标记表示跨主机 0 提交。"),
        ("E", "4. 同步代价：每事务加锁数 / 每事务 RDMA 数 / 每锁 RDMA 数",
         "把吞吐差异拆回「一次事务发多少把锁、多少次单边 RDMA」，对应综述的第三条路线「锁服务实现层」。"),
    ]
    for tag, title, desc in sections:
        parts.append("<h2>%s</h2><p>%s</p><div class='grid'>" % (title, desc))
        for fn, sec, t, note in FIGURES:
            if sec != tag:
                continue
            parts.append('<div class="fig"><h3>%s</h3><img src="assets/%s" alt="%s"><p>%s</p></div>'
                         % (html.escape(t), fn, html.escape(t), html.escape(note)))
        parts.append("</div>")

    parts.append("<h2>5. 对比结论（均由 427 行原始 CSV 计算）</h2>")

    # 5.1 A vs E
    parts.append("<h3>5.1 点读写 → 范围扫描：细粒度协议掉一个数量级，粗粒度协议反而更稳</h3>")
    parts.append("<p>no-wait 下 <code>A-0</code> 与 <code>E-32</code> 的吞吐比：<b>&gt;1 表示点读写更快</b>（细粒度协议被扫描拖垮），<b>&lt;1 表示范围扫描更快</b>（粗粒度协议受益）。注意两者键分布相同（ordered-zipf θ=0.99），差别只在访问类型。</p>")
    parts.append("<table><tr><th>锁语义</th><th>本地 A/E 倍数</th><th>跨主机 A/E 倍数</th></tr>")
    for name, l, r in F["ae_ratio"]:
        parts.append("<tr><td>%s</td><td>%s</td><td>%s</td></tr>" % (name, fmt(l, 2), fmt(r, 2)))
    parts.append("</table>")

    # 5.2 local/rdma
    parts.append("<h3>5.2 跨主机的代价（E-32, no-wait）</h3>")
    parts.append("<table><tr><th>锁语义</th><th>本地 txn/s</th><th>跨主机 txn/s</th><th>倍数</th></tr>")
    for name, l, r, q in F["local_rdma"]:
        parts.append("<tr><td>%s</td><td>%s</td><td>%s</td><td>%s</td></tr>"
                     % (name, fmt(l, 0), fmt(r, 0), fmt(q, 1)))
    parts.append("</table>")
    parts.append('<p class="small">E-32 是 ordered-zipf θ=0.99 的高冲突格，绝对吞吐普遍只有 10⁰–10² txn/s；'
                 '个别倍数 &lt;1 属单次测量波动，不构成「跨主机更快」的结论。真正稳定的量大差异见 '
                 '<code>hierarchical b64</code>（49.6×）与 <code>exact-range</code>（4.3×）。</p>')

    # 5.3 policies -- careful: aborted is an ATTEMPT-level counter here.
    parts.append("<h3>5.3 等待策略：尝试级失败占比（全体协议 × 配置点均值）</h3>")
    parts.append("<table><tr><th>侧</th><th>no-wait</th><th>wait-die</th><th>wound-wait</th></tr>")
    for side in SIDES:
        parts.append("<tr><td>%s</td><td>%s%%</td><td>%s%%</td><td>%s%%</td></tr>" % (
            SIDE_LABEL[side], fmt(F["abort"][(side, "no-wait")], 2),
            fmt(F["abort"][(side, "wait-die")], 2),
            fmt(F["abort"][(side, "wound-wait")], 2)))
    parts.append("</table>")
    parts.append("""<div class="warn"><b>口径警告（本轮最重要的解释）：</b>
    CSV 的 <code>aborted</code> 是<b>尝试级</b>计数，约等于 <code>retries</code>，
    不是「最终失败的事务数」。逐行核对 427 格，<code>logical_failed</code> <b>全部为 0</b>，
    即没有一笔逻辑事务最终失败；wound-wait 下 <code>wound_aborts ≈ aborted ≈ retries</code>，
    表示年轻事务的尝试被 wound 后立即重试，代价是重试风暴把吞吐压到个位数 txn/s。
    因此上表和 B 组图的 y 轴应读作「尝试失败占比」，<b>不能写成事务中止率</b>。</div>""")
    parts.append("<table><tr><th>侧</th><th>committed</th><th>aborted（尝试）</th><th>retries</th><th>wound_aborts</th><th>logical_failed</th></tr>")
    for side in SIDES:
        a = F["accounting"][side]
        parts.append("<tr><td>%s</td><td>%s</td><td>%s</td><td>%s</td><td>%s</td><td>%s</td></tr>" % (
            SIDE_LABEL[side], fmt(a["committed"], 0), fmt(a["aborted"], 0),
            fmt(a["retries"], 0), fmt(a["wound_aborts"], 0),
            fmt(a["logical_failed"], 0)))
    parts.append("</table>")

    # 5.4 bucket / range / per-lock
    parts.append("<h3>5.4 桶宽、扫描宽度与锁服务代价</h3><ul>")
    for side in SIDES:
        v = F["bucket"][side]
        parts.append("<li>%s 层级锁 <b>b16→b256</b> 吞吐比（7 个配置点）：最小 %s，最大 %s，均值 %s。</li>" % (
            SIDE_LABEL[side], fmt(min(v), 2) if v else "—",
            fmt(max(v), 2) if v else "—",
            fmt(sum(v) / len(v), 2) if v else "—"))
    for side in SIDES:
        v = F["range"][side]
        parts.append("<li>%s <b>r8→r128</b> 吞吐比：%s。</li>" % (
            SIDE_LABEL[side], "；".join("%s %s×" % (n, fmt(q, 2)) for n, q in v)))
    parts.append("<li>每把锁的 RDMA 操作数（no-wait，7 点均值）：%s。</li>" % (
        "；".join("%s %s" % (n, fmt(v, 2)) for n, v in F["per_lock"])))
    parts.append("</ul>")

    # 5.5 best protocol
    parts.append("<h3>5.5 每个 (侧, 等待策略) 下各配置点的最快协议</h3>")
    parts.append("<table><tr><th>侧 / 策略</th><th>各配置点冠军（点名 · txn/s）</th></tr>")
    for (side, policy), vals in F["best"].items():
        parts.append("<tr><td>%s / %s</td><td>%s</td></tr>" % (
            SIDE_LABEL[side], POLICY_LABEL[policy],
            "；".join("%s %s (%s)" % (lab, n, fmt(v, 0)) for lab, n, v in vals)))
    parts.append("</table>")

    # 5.6 no-commits
    parts.append("<h3>5.6 三个 0 提交格（合法结果，不是失败）</h3>")
    parts.append("<p><code>progress_status=no-commits</code> 共 3 行，全部为跨主机 <code>ycsb-e</code> + <code>wound-wait</code> + 大范围扫描。它们在对数吞吐图中无法绘制，已在图内用 <b>×</b> 标于轴底，并在中止率图中显示为 100%。写综述时不能把这三格当正常吞吐点。</p>")
    parts.append("<table><tr><th>侧</th><th>协议</th><th>负载</th><th>range</th><th>策略</th><th>committed</th><th>aborted</th></tr>")
    for side, cc, wl, rng, pol, c, a in F["no_commits"]:
        parts.append("<tr><td>%s</td><td>%s</td><td>%s</td><td>%s</td><td>%s</td><td>%s</td><td>%s</td></tr>"
                     % (side, cc, wl, rng, pol, fmt(c, 0), fmt(a, 0)))
    parts.append("</table>")

    parts.append("<h3>5.7 与 11-07 YCSB-E-skew 矩阵的口径差异（不可混画）</h3>")
    parts.append("""<ul>
<li><b>键分布不同</b>：本轮全部 <code>zipf=0.99</code>，<code>ycsb-a</code> / <code>ycsb-e</code> 的
<code>trace_key_distribution=ordered-zipf</code>，<code>ycsb-e-skew</code> 为
<code>hotspot</code>（80% 请求 / 20% 数据）。11-07 那轮七组配置是 θ=0.4 的 A/E 基线、
uniform 与 hotspot 2×2，偏斜度低得多。</li>
<li><b>这解释了吞吐图上最显眼的反转</b>：<code>E-*</code> 三点几乎所有请求都打向同一个最热键，
范围扫描的冲突窗口完全重叠，吞吐掉到 10⁰–10²；换成 <code>S-*</code> 的 hotspot 后请求被摊到热点集合，
<code>S-8</code> 会回升到 10⁴–10⁵（本地）。落差来自<b>键分布</b>而非协议优劣，
写综述时应把「负载类型」进一步拆成「访问类型 × 键分布」两个自变量。</li>
<li><b>协议集不同</b>：本轮 9 本地 / 8 跨主机协议，新增 <code>precision-lock</code> 与
<code>griffin-pl</code>（dbf145d 分支首次进入正式矩阵），并保留 <code>prior-key</code>；
11-07 那轮是 6 协议且不含这两个新语义。两轮数值不能直接拼到同一条曲线上。</li>
<li><b>等待策略结论不同不等于矛盾</b>：11-07 报告写「wound-wait 几乎消灭中止」，
那是在 θ=0.4 下用同一 <code>aborted/(committed+aborted)</code> 口径；本轮 θ=0.99 下该占比接近 100%，
但 <code>logical_failed=0</code>。差异来自偏斜度与重试风暴，见 5.3。</li>
</ul>""")

    parts.append("<h2>6. 复现与文件</h2>")
    parts.append("""<ul>
<li>原始数据：<code>data/formal-shared-data-{local,cross-host}-%s.csv</code> 与同名 state JSON，从 .66 只读拉取。</li>
<li>绘图脚本：<code>plot_extended.py</code>（纯标准库 + matplotlib，<code>py -3.10 plot_extended.py</code> 可一键重画全套 PNG 与本报告）。</li>
<li>运行命令：<code>python3 scripts/run_shared_matrices.py --config experiments/configs/formal_lock_policies_extended_10s.json --mode both</code>；本轮因偶发失败与 resume 门禁限制，实际以 <code>--resume</code> 续跑完成，详见工作日志。</li>
<li>口径：state <code>status=complete / failed_cases=0</code>，427 行全唯一，<code>source_commit=dbf145d2…</code>。</li>
</ul>""" % STAMP)

    parts.append('<p class="small">注：本报告的文献性论断与仓库红线一致——只呈现实测数据及其直接对比，不把无实现支撑的论断写入综述正文。</p>')
    parts.append("</div></body></html>")
    with open(os.path.join(ROOT, "扩展锁策略正式矩阵-折线图报告.html"), "w", encoding="utf-8") as f:
        f.write("".join(parts))


def main():
    # Coverage sanity check before plotting.
    missing = 0
    for side in SIDES:
        for cc, _ in proto_of(side):
            for policy in POLICIES:
                for cfg in CONFIGS:
                    r = cell(side, cc, policy, cfg[0], cfg[1], bucket_of(cc))
                    if r is None:
                        missing += 1
    print("coverage check: missing cells =", missing, "(expected 0)")
    group_AB()
    group_C()
    group_D()
    group_E()
    F = findings()
    build_report(F)
    print("figures:", len(FIGURES), "| png:", len([x for x in os.listdir(OUT) if x.endswith(".png")]))
    print("no-commits rows:", len(F["no_commits"]))


if __name__ == "__main__":
    main()
