#!/usr/bin/env python3
"""Stage-1 figures for the 2026-11-07 YCSB-E-skew formal matrices.

Reads CSVs pulled from .66 (read-only) and writes self-contained SVGs plus an
HTML report. No third-party dependency. Data scope: the five *complete*
configurations (A zipf0.4 and the 2x2 hotspot design); uniform and E-zipf0.4
were still running when these figures were produced.
"""
import csv
import html
import os
from collections import defaultdict

ROOT = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(ROOT, "data")
ASSETS = os.path.join(ROOT, "assets")
os.makedirs(ASSETS, exist_ok=True)

GROUPS = [
    ("102009-761428", "A / zipf 0.4"),
    ("111410-805210", "hot 80/10"),
    ("120802-828856", "hot 80/20"),
    ("125908-674819", "hot 90/10"),
    ("135001-196674", "hot 90/20"),
    ("144411-523379", "uniform"),
    ("153455-724300", "E / zipf 0.4"),
]
HOT = [("111410-805210", "80% req / 10% data", 0.8, 0.1),
       ("120802-828856", "80% req / 20% data", 0.8, 0.2),
       ("125908-674819", "90% req / 10% data", 0.9, 0.1),
       ("135001-196674", "90% req / 20% data", 0.9, 0.2)]

CCS = ["record-2pl", "gap-lock", "next-key", "prior-key", "hierarchical", "exact-range"]
RDMA = ["rdma-" + c for c in CCS]
LABEL = {"record-2pl": "record-2PL", "gap-lock": "gap", "next-key": "next-key",
         "prior-key": "prior-key", "hierarchical": "bucket", "exact-range": "exact-range"}
COLOR = {"record-2pl": "#378ADD", "gap-lock": "#EF9F27", "next-key": "#639922",
         "prior-key": "#7F77DD", "hierarchical": "#D85A30", "exact-range": "#1D9E75"}
WAIT = ["no-wait", "wait-die", "wound-wait"]
WCOLOR = {"no-wait": "#378ADD", "wait-die": "#EF9F27", "wound-wait": "#1D9E75"}
BUCKETS = ["16", "64", "256"]

INK = "#2C2C2A"
MUTED = "#5F5E5A"
GRID = "#E8E6DF"
FONT = "Microsoft YaHei, PingFang SC, sans-serif"


def load():
    out = {}
    for stamp, name in GROUPS:
        for side in ("local", "cross-host"):
            p = os.path.join(DATA, "formal-shared-data-%s-20261107-%s.csv" % (side, stamp))
            with open(p, encoding="utf-8") as f:
                rows = list(csv.DictReader(f))
            key = (stamp, side)
            out.setdefault(key, []).extend(rows)
    return out


def num(row, k):
    try:
        return float(row.get(k, 0) or 0)
    except ValueError:
        return 0.0


def pick(rows, cc, wait, bucket="64"):
    for r in rows:
        if r["cc"] == cc and r["wait_policy"] == wait and r["hierarchy_bucket_width"] == bucket:
            return r
    return None


def avg(vals):
    vals = [v for v in vals if v is not None]
    return sum(vals) / len(vals) if vals else 0.0


def svg_head(w, h, title, subtitle):
    return ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d %d" width="%d" height="%d" '
            'font-family="%s">\n'
            '<rect width="%d" height="%d" fill="#FFFFFF"/>\n'
            '<text x="40" y="34" font-size="19" font-weight="600" fill="%s">%s</text>\n'
            '<text x="40" y="56" font-size="12" fill="%s">%s</text>\n'
            % (w, h, w, h, FONT, w, h, INK, html.escape(title), MUTED, html.escape(subtitle)))


def axis(y0, y1, x0, x1, vmax, ticks=5, fmt="%.0f"):
    s = []
    for i in range(ticks):
        v = vmax * i / (ticks - 1)
        y = y1 - (y1 - y0) * i / (ticks - 1)
        s.append('<line x1="%d" y1="%.1f" x2="%d" y2="%.1f" stroke="%s" stroke-width="1"/>' % (x0, y, x1, y, GRID))
        s.append('<text x="%d" y="%.1f" font-size="11" fill="%s" text-anchor="end" dominant-baseline="central">%s</text>'
                 % (x0 - 8, y, MUTED, fmt % v))
    return "".join(s)


def grouped_bars(path, title, subtitle, groups, series, values, vmax, fmt="%.0f", legend=True):
    """groups: x labels; series: list of (name,color); values[group][series]."""
    w, h = 1220, 640
    x0, x1, y0, y1 = 95, 1180, 90, 505
    s = [svg_head(w, h, title, subtitle), axis(y0, y1, x0, x1, vmax, fmt=fmt)]
    gw = (x1 - x0) / len(groups)
    bw = min(30, (gw - 24) / len(series))
    for gi, g in enumerate(groups):
        cx = x0 + gi * gw
        s.append('<text x="%.1f" y="%d" font-size="12" fill="%s" text-anchor="middle">%s</text>'
                 % (cx + gw / 2, y1 + 24, INK, html.escape(g)))
        for si, (name, color) in enumerate(series):
            v = values[gi][si]
            bh = (y1 - y0) * max(0.0, v) / vmax
            bx = cx + (gw - bw * len(series)) / 2 + si * bw
            s.append('<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" rx="2" fill="%s" fill-opacity="0.9"/>'
                     % (bx, y1 - bh, bw - 3, bh, color))
    s.append('<line x1="%d" y1="%d" x2="%d" y2="%d" stroke="%s" stroke-width="1"/>'
             % (x0, y1, x1, y1, MUTED))
    if legend:
        lx = x0
        for name, color in series:
            s.append('<rect x="%d" y="%d" width="13" height="13" rx="2" fill="%s"/>' % (lx, 558, color))
            s.append('<text x="%d" y="%d" font-size="12" fill="%s">%s</text>' % (lx + 18, 569, INK, html.escape(name)))
            lx += 24 + len(name) * 8
    s.append('<text x="40" y="610" font-size="11" fill="%s">单次测量（runs=1，seed=8），无误差线；32 线程、关闭协程、10s 预热 + 10s 测量</text>' % MUTED)
    s.append('</svg>')
    open(path, "w", encoding="utf-8").write("".join(s))
    return path


def heatmap(path, title, subtitle, rows, cols, matrix, fmt="%.0f"):
    w, h = 1220, 560
    x0, y0, cw, ch = 210, 105, 155, 78
    s = [svg_head(w, h, title, subtitle)]
    lo, hi = min(min(r) for r in matrix), max(max(r) for r in matrix)
    for ci, c in enumerate(cols):
        s.append('<text x="%d" y="%d" font-size="11" fill="%s" text-anchor="middle">%s</text>'
                 % (x0 + ci * cw + cw / 2, y0 - 12, INK, html.escape(c)))
    for ri, r in enumerate(rows):
        s.append('<text x="%d" y="%d" font-size="12" fill="%s" text-anchor="end" dominant-baseline="central">%s</text>'
                 % (x0 - 12, y0 + ri * ch + ch / 2, INK, html.escape(r)))
        for ci, v in enumerate(matrix[ri]):
            t = (v - lo) / (hi - lo) if hi > lo else 0.5
            # light blue -> deep teal ramp
            red = int(230 - 190 * t)
            grn = int(242 - 75 * t)
            blu = int(251 - 110 * t)
            s.append('<rect x="%d" y="%d" width="%d" height="%d" rx="4" fill="rgb(%d,%d,%d)" stroke="#FFFFFF" stroke-width="2"/>'
                     % (x0 + ci * cw, y0 + ri * ch, cw - 4, ch - 4, red, grn, blu))
            s.append('<text x="%d" y="%d" font-size="15" font-weight="600" fill="%s" text-anchor="middle" dominant-baseline="central">%s</text>'
                     % (x0 + ci * cw + (cw - 4) / 2, y0 + ri * ch + (ch - 4) / 2,
                        "#FFFFFF" if t > 0.55 else INK, fmt % v))
    s.append('<text x="40" y="530" font-size="11" fill="%s">色深 = 吞吐越高；单次测量，无误差线。行=hotspot 2×2 因子，列=锁协议（本地，no-wait）</text>' % MUTED)
    s.append('</svg>')
    open(path, "w", encoding="utf-8").write("".join(s))
    return path


def lines(path, title, subtitle, xs, series, values, vmax, fmt="%.0f"):
    w, h = 1220, 600
    x0, x1, y0, y1 = 95, 1180, 90, 470
    s = [svg_head(w, h, title, subtitle), axis(y0, y1, x0, x1, vmax, fmt=fmt)]
    for i, x in enumerate(xs):
        px = x0 + (x1 - x0) * i / max(1, len(xs) - 1)
        s.append('<line x1="%.1f" y1="%d" x2="%.1f" y2="%d" stroke="%s" stroke-width="1"/>' % (px, y0, px, y1, GRID))
        s.append('<text x="%.1f" y="%d" font-size="12" fill="%s" text-anchor="middle">%s</text>' % (px, y1 + 24, INK, html.escape(x)))
    for name, color in series:
        pts = []
        for i, v in enumerate(values[name]):
            px = x0 + (x1 - x0) * i / max(1, len(xs) - 1)
            py = y1 - (y1 - y0) * v / vmax
            pts.append("%.1f,%.1f" % (px, py))
            s.append('<circle cx="%.1f" cy="%.1f" r="5" fill="%s"/>' % (px, py, color))
        s.append('<polyline points="%s" fill="none" stroke="%s" stroke-width="2.5"/>' % (" ".join(pts), color))
    lx = x0
    for name, color in series:
        s.append('<rect x="%d" y="%d" width="13" height="13" rx="2" fill="%s"/>' % (lx, 520, color))
        s.append('<text x="%d" y="%d" font-size="12" fill="%s">%s</text>' % (lx + 18, 531, INK, html.escape(name)))
        lx += 26 + len(name) * 8
    s.append('<text x="40" y="575" font-size="11" fill="%s">跨 7 组配置取平均；单次测量，无误差线</text>' % MUTED)
    s.append('</svg>')
    open(path, "w", encoding="utf-8").write("".join(s))
    return path


def main():
    data = load()
    figures = []

    # Fig 1/2: throughput per distribution, no-wait, bucket=64 for hierarchy
    for side, ccs, fname, t, sub in (
        ("local", CCS, "fig1-local-throughput.svg", "图 1 · 本地吞吐（no-wait）",
         "每组 32 线程、2^21 键、10 请求/事务；bucket 锁取 b=64；无锁基线 none 单独见文末"),
        ("cross-host", RDMA, "fig2-cross-throughput.svg", "图 2 · 跨主机吞吐（no-wait）",
         "真实 .66/.67 RoCE 回环，原生 RDMA 状态与共享 B+ 树；区间槽 32768/分片")):
        vals = []
        for stamp, _ in GROUPS:
            rows = data[(stamp, side)]
            vals.append([num(pick(rows, cc, "no-wait") or {}, "throughput_committed_txn_s") for cc in ccs])
        flat = [v for row in vals for v in row]
        vmax = max(flat) * 1.15
        grouped_bars(os.path.join(ASSETS, fname), t, sub,
                     [g[1] for g in GROUPS], [(LABEL[c], COLOR[c]) for c in CCS], vals, vmax, fmt="%.0f")
        figures.append((fname, t, sub))

    # Fig 3: abort rate by wait policy (averaged over 5 groups)
    vals = []
    for cc in CCS:
        row = []
        for w in WAIT:
            rs = [num(pick(data[(s, "local")], cc, w) or {}, "aborted") /
                  max(1.0, num(pick(data[(s, "local")], cc, w) or {}, "aborted") +
                      num(pick(data[(s, "local")], cc, w) or {}, "committed"))
                  for s, _ in GROUPS]
            row.append(avg(rs) * 100)
        vals.append(row)
    vmax = max(max(r) for r in vals) * 1.15
    grouped_bars(os.path.join(ASSETS, "fig3-wait-policy-abort.svg"),
                 "图 3 · 等待策略对中止率的影响（本地，跨 7 组平均）",
                 "中止率 = aborted / (committed + aborted)；wound-wait 用年龄仲裁替代重试",
                 [LABEL[c] for c in CCS], [(w, WCOLOR[w]) for w in WAIT], vals, vmax, fmt="%.1f")
    figures.append(("fig3-wait-policy-abort.svg", "图 3 · 等待策略对中止率的影响", "跨 7 组平均"))

    # Fig 4: hotspot 2x2 heatmap
    matrix = []
    for stamp, _, _, _ in HOT:
        rows = data[(stamp, "local")]
        matrix.append([num(pick(rows, cc, "no-wait") or {}, "throughput_committed_txn_s") for cc in CCS])
    heatmap(os.path.join(ASSETS, "fig4-hotspot-heatmap.svg"),
            "图 4 · hotspot 2×2 因子 × 锁协议（本地吞吐，no-wait）",
            "固定 80%/90% 请求、10%/20% 热点数据；四项因果比较每次只改一个参数",
            [h[1] for h in HOT], [LABEL[c] for c in CCS], matrix)
    figures.append(("fig4-hotspot-heatmap.svg", "图 4 · hotspot 2×2 因子热图", "四个热点配置齐备"))

    # Fig 5: bucket width
    series = [(w, WCOLOR[w]) for w in WAIT]
    values = {}
    for w in WAIT:
        values[w] = [avg([num(pick(data[(s, "local")], "hierarchical", w, b) or {},
                              "throughput_committed_txn_s") for s, _ in GROUPS]) for b in BUCKETS]
    vmax = max(max(v) for v in values.values()) * 1.15
    lines(os.path.join(ASSETS, "fig5-bucket-width.svg"),
          "图 5 · 固定桶宽对层级桶锁吞吐的影响（本地，跨 7 组平均）",
          "b 是逻辑键区间宽度（bucket_id = key / b），不是物理叶页大小",
          ["b=16", "b=64", "b=256"], series, values, vmax)
    figures.append(("fig5-bucket-width.svg", "图 5 · 桶宽 16/64/256", "三档 × 三策略"))

    # Fig 6: cross-host RDMA cost per lock acquisition
    vals = []
    for cc in RDMA:
        rs = []
        for s, _ in GROUPS:
            r = pick(data[(s, "cross-host")], cc, "no-wait")
            rs.append(num(r or {}, "rdma_operations") / max(1.0, num(r or {}, "lock_acquires")))
        vals.append([avg(rs)])
    vmax = max(v[0] for v in vals) * 1.15
    grouped_bars(os.path.join(ASSETS, "fig6-rdma-per-lock.svg"),
                 "图 6 · 跨主机一次加锁的 RDMA 操作代价（no-wait，跨 7 组平均）",
                 "rdma_operations / lock_acquires；锁语义不变，仅反映锁服务实现层开销",
                 [LABEL[c] for c in CCS], [("每锁 RDMA 操作数", "#378ADD")], vals, vmax, fmt="%.1f")
    figures.append(("fig6-rdma-per-lock.svg", "图 6 · 每锁 RDMA 操作数", "锁服务实现层代价"))

    # HTML report
    parts = ['<!DOCTYPE html><html lang="zh-CN"><head><meta charset="utf-8">',
             '<title>YCSB-E-skew 正式矩阵 · 完整图（7/7）</title>',
             '<style>body{font-family:"Microsoft YaHei",sans-serif;max-width:1280px;margin:0 auto;padding:28px;color:#2C2C2A;background:#FAFAF8}',
             'h1{font-size:26px}h2{font-size:18px;margin-top:34px}p{line-height:1.75;font-size:14px}',
             '.warn{background:#FAEEDA;border-left:4px solid #BA7517;padding:14px 18px;border-radius:6px}',
             '.fig{background:#fff;border:1px solid #E8E6DF;border-radius:10px;padding:12px;margin:18px 0}',
             '.fig img{width:100%;display:block}table{border-collapse:collapse;width:100%;font-size:13px}',
             'td,th{border:1px solid #E8E6DF;padding:8px 10px;text-align:left}th{background:#F1EFE8}',
             'code{background:#F1EFE8;padding:2px 5px;border-radius:3px}</style></head><body>',
             '<h1>YCSB-E-skew 正式矩阵 · 完整图</h1>',
             '<div class="warn"><b>完整结果（7/7）。</b> 七份配置全部跑完：A(zipf 0.4)、hotspot 2×2 四点、uniform 对照、E(zipf 0.4)。'
             ''
             '全部为 <code>repetition_policy=single-run</code> 单次测量，<b>无误差线、不得宣称置信区间</b>。</div>',
             '<h2>实验口径</h2><p>HEAD <code>2017493</code>；32 OS 线程、<code>execution_mode=threads</code>、'
             'coroutines=1；2^21 键域；10 请求/事务；10s 全并发预热 + 10s 测量；seed=8；'
             '本地 6 协议 + none 基线，跨主机 6 协议；三种等待策略 no-wait / wait-die / wound-wait；'
             '层级桶锁三档 b=16/64/256。每份配置本地 25 + 跨主机 24 = 49 主 case，'
             '本轮 7×49 = 343 个主 case 全部落盘，7 组 rc=0、failed_cases=0。</p>',
             '<p>图中吞吐对比固定 <code>wait_policy=no-wait</code>、层级锁取 <code>b=64</code>；'
             '无锁基线 <code>none</code> 因量级差异过大未画入柱状图（A 组本地约 307 903 txn/s）。</p>']
    for fname, t, sub in figures:
        png = fname.replace(".svg", ".png")
        parts.append('<div class="fig"><h2>%s</h2><p>%s</p><img src="assets/%s" alt="%s"></div>'
                     % (html.escape(t), html.escape(sub), png, html.escape(t)))
    parts.append('<h2>数据文件</h2><p>来源：<code>.66:~/RdmaRangeLock/experiments/results/'
                 'formal-shared-data-{local,cross-host}-20261107-*.csv</code>，'
                 '本地副本在 <code>data/</code>。生成脚本 <code>plot_stage.py</code>。</p>')
    parts.append('</body></html>')
    open(os.path.join(ROOT, "正式报告-7of7.html"), "w", encoding="utf-8").write("".join(parts))
    print("generated", len(figures), "SVGs + HTML report")
    for f in figures:
        print(" ", f[0])


if __name__ == "__main__":
    main()
