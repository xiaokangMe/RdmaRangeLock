#!/usr/bin/env python3
"""Line charts for the 2026-11-07 YCSB-E-skew formal matrices (final 7/7 run).

Style is copied from .66:~/RdmaRangeLock/scripts/plot_cross_host.py and
plot_zipf_predicate.py: matplotlib Agg, one axes per PNG, marker="o" ms=4.5
lw=1.8, dotted major grid, top/right spines hidden, log y with 10^k ticks,
dpi=170 white face, and the project's protocol / policy / bucket colours.

Unlike the sweep scripts the x axis here is the configuration series (the
2026-11-07 campaign has no five-point Zipf theta scan); interaction figures
use the genuinely continuous hotspot request fraction instead.
"""
import csv
import glob
import html
import math
import os
from collections import defaultdict

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

ROOT = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(ROOT, "data")
OUT = os.path.join(ROOT, "assets", "lines")
os.makedirs(OUT, exist_ok=True)

plt.rcParams.update({
    "figure.dpi": 140, "savefig.dpi": 170, "font.size": 9,
    "legend.frameon": False, "axes.unicode_minus": False,
    "font.sans-serif": ["Microsoft YaHei", "DejaVu Sans"],
})

GROUPS = [
    ("102009-761428", "A z.4"),
    ("111410-805210", "80/10"),
    ("120802-828856", "80/20"),
    ("125908-674819", "90/10"),
    ("135001-196674", "90/20"),
    ("144411-523379", "uniform"),
    ("153455-724300", "E z.4"),
]
HOT = [(0.8, 0.1, "111410-805210"), (0.8, 0.2, "120802-828856"),
       (0.9, 0.1, "125908-674819"), (0.9, 0.2, "135001-196674")]

PROTO = [
    ("record-2pl", "record 2PL", "#1f77b4"),
    ("gap-lock", "gap lock", "#ff7f0e"),
    ("next-key", "next-key", "#2ca02c"),
    ("prior-key", "prior-key", "#6A4C93"),
    ("hierarchical", "bucket b64", "#d62728"),
    ("exact-range", "exact-range", "#2A9D8F"),
]
POLICIES = ["no-wait", "wait-die", "wound-wait"]
POLICY_COLOUR = {"no-wait": "#d62728", "wait-die": "#1f77b4", "wound-wait": "#2ca02c"}
BUCKET = [(16, "b16", "#f4a3a1"), (64, "b64", "#e06663"), (256, "b256", "#b3241f")]
XS = list(range(len(GROUPS)))
XLAB = [g[1] for g in GROUPS]


def read():
    idx = defaultdict(list)
    for side in ("local", "cross-host"):
        for path in glob.glob(os.path.join(DATA, "formal-shared-data-%s-20261107-*.csv" % side)):
            stamp = path.rsplit("-", 2)[-2] + "-" + path.rsplit("-", 1)[-1].replace(".csv", "")
            with open(path, newline="", encoding="utf-8") as f:
                for r in csv.DictReader(f):
                    idx[(stamp, side, r["cc"], r["wait_policy"],
                         r["hierarchy_bucket_width"])].append(r)
    return idx


IDX = read()


def fnum(r, k):
    try:
        return float(r.get(k, 0) or 0)
    except ValueError:
        return 0.0


def cell(stamp, side, cc, policy="no-wait", bucket=64):
    return (IDX.get((stamp, side, cc, policy, str(bucket))) or [None])[0]


def metric(stamp, side, cc, policy, bucket, kind):
    r = cell(stamp, side, cc, policy, bucket)
    if r is None:
        return None
    tx = fnum(r, "transactions")
    if kind == "throughput":
        return fnum(r, "throughput_committed_txn_s")
    if kind == "abort":
        return 100.0 * fnum(r, "aborted") / tx if tx else None
    if kind == "locks":
        return fnum(r, "lock_acquires") / tx if tx else None
    if kind == "rdma":
        return fnum(r, "rdma_operations") / tx if tx else None
    raise ValueError(kind)


def clean(ys):
    return [float("nan") if v is None else v for v in ys]


def style(ax, title, ylabel, logy, xlab=XLAB, ylim=None):
    ax.set_xticks(XS[:len(xlab)])
    ax.set_xticklabels(xlab, fontsize=8)
    ax.set_xlim(-0.3, len(xlab) - 0.7)
    if logy:
        ax.set_yscale("log")
        lo, hi = ax.get_ylim()
        k0 = int(math.floor(math.log10(max(lo, 1e-3))))
        k1 = int(math.ceil(math.log10(max(hi, 1e-2))))
        ticks = [10.0 ** k for k in range(k0, k1 + 1)]

        def label(v):
            if v >= 1e6:
                return "%gM" % (v / 1e6)
            if v >= 1e3:
                return "%gk" % (v / 1e3)
            return "%g" % v
        ax.set_yticks(ticks)
        ax.set_yticklabels([label(v) for v in ticks], fontsize=9)
        ax.minorticks_off()
    if ylim:
        ax.set_ylim(*ylim)
    ax.set_xlabel("configuration series (A -> hotspot 2x2 -> uniform -> E z.4)", fontsize=9)
    ax.set_ylabel(ylabel, fontsize=10)
    ax.set_title(title, fontsize=11, pad=8)
    ax.grid(True, which="major", ls=":", lw=0.5, alpha=0.6)
    ax.set_axisbelow(True)
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)


def save(fig, name):
    fig.tight_layout()
    fig.savefig(os.path.join(OUT, name), dpi=170, facecolor="white")
    plt.close(fig)
    print("wrote", name)


UNIT = {"throughput": "committed txn/s", "abort": "aborted share (%)",
        "locks": "locks per transaction", "rdma": "RDMA ops per transaction"}


def chart_protocols(side, kind, tag):
    fig, ax = plt.subplots(figsize=(6.4, 4.3))
    drawn = 0
    for cc, label, colour in PROTO:
        real = ("rdma-" + cc) if side == "cross-host" else cc
        ys = [metric(s, side, real, "no-wait", 64, kind) for s, _ in GROUPS]
        if ys[0] is None:
            continue
        ax.plot(XS, clean(ys), marker="o", ms=4.5, lw=1.8, color=colour, label=label)
        drawn += 1
    style(ax, "%s · %s · %s" % (tag, kind, UNIT[kind]), UNIT[kind], kind in ("throughput", "locks", "rdma"))
    if kind == "abort":
        ax.set_ylim(0, 100)
    if drawn:
        ax.legend(fontsize=8, frameon=False, loc="best")
    save(fig, "%s-%s.png" % (kind, "rdma" if side == "cross-host" else "local"))


def chart_policy(cc, label):
    fig, ax = plt.subplots(figsize=(6.4, 4.3))
    for p in POLICIES:
        ys = [metric(s, "local", cc, p, 64, "throughput") for s, _ in GROUPS]
        ax.plot(XS, clean(ys), marker="o", ms=4.5, lw=1.8,
                color=POLICY_COLOUR[p], label=p)
    style(ax, "%s · wait policies (local)" % label, "committed txn/s", True)
    ax.legend(fontsize=8, frameon=False, loc="best")
    save(fig, "policy-%s.png" % cc)


def chart_buckets():
    fig, ax = plt.subplots(figsize=(6.4, 4.3))
    for b, label, colour in BUCKET:
        ys = [metric(s, "local", "hierarchical", "no-wait", b, "throughput") for s, _ in GROUPS]
        ax.plot(XS, clean(ys), marker="s", ms=4, lw=1.8, color=colour, label=label)
    style(ax, "bucket width vs throughput (local, no-wait)", "committed txn/s", True)
    ax.legend(fontsize=8, frameon=False, loc="best")
    save(fig, "bucket-widths.png")


def chart_compare():
    fig, ax = plt.subplots(figsize=(6.4, 4.3))
    for cc, label, colour in PROTO:
        if cc not in ("record-2pl", "hierarchical", "exact-range"):
            continue
        ly = [metric(s, "local", cc, "no-wait", 64, "throughput") for s, _ in GROUPS]
        ry = [metric(s, "cross-host", "rdma-" + cc, "no-wait", 64, "throughput") for s, _ in GROUPS]
        ax.plot(XS, clean(ly), marker="o", ms=4.5, lw=1.8, color=colour, label="%s local" % label)
        ax.plot(XS, clean(ry), marker="s", ms=4.2, lw=1.8, ls="--", color=colour, label="%s RDMA" % label)
    style(ax, "local vs RDMA · throughput", "committed txn/s", True)
    ax.legend(fontsize=7.4, frameon=False, loc="lower left", ncol=2)
    save(fig, "compare-local-rdma.png")


def chart_interaction(cc, label):
    fig, ax = plt.subplots(figsize=(6.0, 4.3))
    ys_all = []
    for frac, colour in ((0.1, "#1f77b4"), (0.2, "#d62728")):
        ys = []
        for op in (0.8, 0.9):
            stamp = next(s for o, f, s in HOT if o == op and f == frac)
            ys.append(metric(stamp, "local", cc, "no-wait", 64, "throughput"))
        ys_all.extend(ys)
        ax.plot([0.8, 0.9], clean(ys), marker="o", ms=5, lw=1.8, color=colour,
                label="hot data %.0f%%" % (frac * 100))
    u = metric("144411-523379", "local", cc, "no-wait", 64, "throughput")
    if u:
        ax.axhline(u, ls=":", lw=1.3, color="#888780", label="uniform reference")
        allv = [v for v in ys_all if v is not None] + [u]
        ax.set_ylim(min(allv) * 0.97, u * 1.04)
    ax.set_xticks([0.8, 0.9])
    ax.set_xticklabels(["80%", "90%"])
    ax.set_xlabel("hotspot request fraction", fontsize=10)
    ax.set_ylabel("committed txn/s", fontsize=10)
    ax.set_title("%s · hotspot 2x2 interaction (local)" % label, fontsize=11, pad=8)
    ax.set_yscale("log")
    ax.grid(True, which="major", ls=":", lw=0.5, alpha=0.6)
    ax.set_axisbelow(True)
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)
    ax.legend(fontsize=8, frameon=False, loc="best")
    save(fig, "interaction-%s.png" % cc)


def write_report(names):
    css = ('body{font-family:"Microsoft YaHei",sans-serif;max-width:1180px;margin:0 auto;padding:26px;'
           'color:#2C2C2A;background:#FAFAF8}h1{font-size:25px}h2{font-size:17px;margin-top:26px}'
           'p{line-height:1.75;font-size:14px}.warn{background:#FAEEDA;border-left:4px solid #BA7517;'
           'padding:13px 17px;border-radius:6px}.fig{background:#fff;border:1px solid #E8E6DF;'
           'border-radius:10px;padding:10px;margin:14px 0}.fig img{width:100%;display:block}'
           'code{background:#F1EFE8;padding:2px 5px;border-radius:3px}')
    p = ['<!DOCTYPE html><html lang="zh-CN"><head><meta charset="utf-8">',
         '<title>YCSB-E-skew 折线图（7/7）</title><style>%s</style></head><body>' % css,
         '<h1>YCSB-E-skew 正式矩阵 · 折线图（项目脚本风格）</h1>',
         '<div class="warn"><b>完整结果（7/7）。</b> 七份配置全部完成，7 组 rc=0、failed_cases=0、343 个主 case 全部落盘。'
         '全部 <code>single-run</code> 单次测量，<b>无误差线、不得宣称置信区间</b>。</div>',
         '<p>按 .66 上 <code>scripts/plot_cross_host.py</code> 与 <code>plot_zipf_predicate.py</code> 的规范绘制：'
         'matplotlib Agg、一张 PNG 一个 axes、<code>marker="o" ms=4.5 lw=1.8</code>、点线网格、'
         '隐藏上/右边框、对数纵轴 10^k 刻度、dpi 170 白底，配色沿用项目的协议/策略/桶宽色表。'
         '本地 <code>py -3.10</code> 的 matplotlib 3.10.9 生成，数据来自只读拉取的 CSV。</p>',
         '<p><b>与旧扫描脚本的差异</b>：那些脚本的 x 轴是 Zipf θ 五档（0/0.4/0.5/0.7/0.9），'
         '本轮正式矩阵没有 θ 扫描，主自变量换成 7 份配置序列（A → hotspot 2×2 → uniform → E zipf0.4）；'
         '交互作用图则用真正连续的「热点请求比例 80%→90%」，虚线为 uniform 对照。'
         '配置序列是序数轴，折线表示轨迹而非数值插值。</p>']
    for name, title in names:
        p.append('<div class="fig"><h2>%s</h2><img src="assets/lines/%s" alt="%s"></div>'
                 % (html.escape(title), name, html.escape(title)))
    p.append('<p>脚本 <code>plot_lines_mpl.py</code>；柱状图版见 <code>正式报告-7of7.html</code>。</p></body></html>')
    open(os.path.join(ROOT, "折线图报告-7of7.html"), "w", encoding="utf-8").write("".join(p))


def main():
    names = []
    for side, tag in (("local", "local"), ("cross-host", "RDMA")):
        for kind in ("throughput", "abort", "locks", "rdma"):
            if side == "local" and kind == "rdma":
                continue
            chart_protocols(side, kind, tag)
            n = "%s-%s.png" % (kind, "rdma" if side == "cross-host" else "local")
            names.append((n, "%s · %s (%s)" % (kind, tag, "跨主机" if side == "cross-host" else "本地")))
    for cc, label in (("record-2pl", "record 2PL"), ("hierarchical", "bucket b64"),
                      ("exact-range", "exact-range")):
        chart_policy(cc, label)
        names.append(("policy-%s.png" % cc, "等待策略曲线 · %s" % label))
    chart_buckets()
    names.append(("bucket-widths.png", "桶宽曲线 b16 / b64 / b256"))
    chart_compare()
    names.append(("compare-local-rdma.png", "本地 vs 跨主机吞吐对比"))
    for cc, label in (("record-2pl", "record 2PL"), ("hierarchical", "bucket b64"),
                      ("exact-range", "exact-range")):
        chart_interaction(cc, label)
        names.append(("interaction-%s.png" % cc, "hotspot 2×2 交互 · %s" % label))
    write_report(names)
    print("total", len(names), "line charts")


if __name__ == "__main__":
    main()
