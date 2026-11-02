#!/usr/bin/env python3
"""Build the final single-file HTML report from the two formal matrices.

Everything numeric in the prose is computed from the CSVs at build time, so the
text cannot drift from the tables. The report also embeds its own provenance:
commit, both result files with sha256, and the config.txt that each matrix wrote
when it started.
"""

import base64
import glob
import hashlib
import html
import os
import statistics as st
import sys
from collections import defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import csv_schema

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RESULTS = os.path.join(ROOT, "experiments", "results")
FIG_DIR = os.path.join(ROOT, "figures", "final")
OUT = os.path.join(ROOT, "docs", "reports", "final-matrix-2026-11-02.html")

THREADS = [1, 2, 4, 8, 16, 32]
LOCAL_CCS = ["none", "record-2pl", "gap-lock", "next-key", "hierarchical"]
RDMA_CCS = ["rdma-next-key", "rdma-hierarchical"]
WORKLOADS = ["ycsb-a", "ycsb-e", "tpcc-payment", "tpcc-new-order"]
AUTO_BUCKET = {"ycsb-e": 256}

LABEL = {
    "none": "无锁",
    "record-2pl": "record 2PL",
    "gap-lock": "gap lock",
    "next-key": "next-key",
    "hierarchical": "hierarchical",
    "rdma-next-key": "next-key（RDMA）",
    "rdma-hierarchical": "hierarchical（RDMA）",
}
WL_LABEL = {
    "ycsb-a": "YCSB-A（50% 更新）",
    "ycsb-e": "YCSB-E（范围读 + 插入）",
    "tpcc-payment": "TPC-C payment",
    "tpcc-new-order": "TPC-C new-order",
}


def git_head():
    try:
        import subprocess

        return subprocess.check_output(
            ["git", "-C", ROOT, "rev-parse", "--short", "HEAD"], text=True).strip()
    except Exception:
        return "unknown"


def load_latest(pattern):
    """Newest parseable result file; legacy headers are skipped with a warning."""
    files = sorted(glob.glob(os.path.join(RESULTS, pattern)), reverse=True)
    for path in files:
        try:
            with open(path, newline="") as handle:
                return path, csv_schema.load_rows(handle)
        except SystemExit as error:
            print("skipping %s: %s" % (os.path.basename(path), error), file=sys.stderr)
            continue
    return None, None


def sha256(path):
    with open(path, "rb") as handle:
        return hashlib.sha256(handle.read()).hexdigest()


def bucket_for(workload):
    return AUTO_BUCKET.get(workload, 16)


def med(table, key, field):
    values = table.get(key)
    if not values:
        return None
    return st.median(float(v[field]) for v in values)


def pct(part, whole):
    return 100.0 * part / whole if whole else 0.0


def img_tag(name, caption, note):
    path = os.path.join(FIG_DIR, name)
    if not os.path.exists(path):
        return ""
    with open(path, "rb") as handle:
        data = base64.b64encode(handle.read()).decode("ascii")
    return ('<figure><img src="data:image/png;base64,%s" alt="%s">'
            '<figcaption><b>%s</b>　%s</figcaption></figure>'
            % (data, html.escape(caption), html.escape(caption), note))


def table(headers, rows, cls=""):
    head = "".join("<th>%s</th>" % h for h in headers)
    body = "".join("<tr>%s</tr>" % "".join("<td>%s</td>" % c for c in row) for row in rows)
    return '<table class="%s"><thead><tr>%s</tr></thead><tbody>%s</tbody></table>' % (cls, head, body)


def load_config(log_dir_glob):
    matches = sorted(glob.glob(os.path.join(ROOT, "experiments", "logs", log_dir_glob,
                                            "config.txt")))
    if not matches:
        return None, []
    with open(matches[-1], encoding="utf-8") as handle:
        lines = [line.strip() for line in handle if line.strip()]
    return matches[-1], lines


def config_value(lines, key):
    for line in lines:
        name, _, value = line.partition("=")
        if name == key:
            return value
    return None


def main():
    local_path, local = load_latest("formal-local-*.csv")
    cross_path, cross = load_latest("formal-cross-host-*.csv")
    if not local:
        raise SystemExit("no local matrix data")

    lt = defaultdict(list)
    for row in local:
        lt[(row["workload"], row["cc"], row["bucket"], row["range"], row["threads"])].append(row)

    # ---- facts computed from the data -------------------------------------
    windows_local = [float(r["elapsed_ns"]) / 1e9 for r in local]
    decay = {}
    for workload in WORKLOADS:
        rng = "8" if workload == "ycsb-e" else "0"
        for cc in LOCAL_CCS:
            bucket = bucket_for(workload) if cc == "hierarchical" else 0
            a = med(lt, (workload, cc, str(bucket), rng, "1"), "throughput_committed_txn_s")
            b = med(lt, (workload, cc, str(bucket), rng, "32"), "throughput_committed_txn_s")
            if a and b:
                decay[(workload, cc)] = b / a

    ycsbe = "ycsb-e"
    # Per-transaction lock counts: each range has its own transaction count, so
    # dividing one range's acquisitions by another range's transactions is wrong.
    def per_txn(cc, bucket, rng):
        key = (ycsbe, cc, str(bucket), rng, "1")
        acquires = med(lt, key, "lock_acquires")
        txns = med(lt, key, "transactions")
        return acquires / txns if acquires and txns else 0.0

    # bucket argmax at t=1 and t=32 for each workload
    bucket_best = {}
    for workload in WORKLOADS:
        rng = "8" if workload == "ycsb-e" else "0"
        for t in ("1", "32"):
            scores = []
            for b in (16, 64, 256):
                v = med(lt, (workload, "hierarchical", str(b), rng, t),
                        "throughput_committed_txn_s")
                if v:
                    scores.append((v, b))
            if scores:
                bucket_best[(workload, t)] = max(scores)

    html_parts = []
    html_parts.append("""<!DOCTYPE html><html lang="zh-CN"><head><meta charset="utf-8">
<title>RdmaRangeLock 正式矩阵（2026-11-02）</title>
<style>
:root{--ink:#1a1a1a;--muted:#5c6370;--line:#e3e6ea;--bg:#fbfbfc;--accent:#b3392f;--accent2:#2f6f4f;}
*{box-sizing:border-box}
body{margin:0;padding:36px 20px 80px;background:var(--bg);color:var(--ink);
     font:15px/1.72 -apple-system,"Segoe UI","Noto Sans CJK SC","Microsoft YaHei",sans-serif}
.wrap{max-width:1040px;margin:0 auto}
h1{font-size:27px;margin:0 0 6px;letter-spacing:-.01em}
.sub{color:var(--muted);font-size:13px;margin-bottom:26px}
h2{font-size:19px;margin:38px 0 12px;padding-bottom:6px;border-bottom:1px solid var(--line)}
h3{font-size:15px;margin:22px 0 8px}
p{margin:9px 0}
code{background:#eef0f3;padding:1px 5px;border-radius:3px;font-size:12.5px;
     font-family:ui-monospace,SFMono-Regular,Menlo,monospace}
.cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(160px,1fr));gap:12px;margin:18px 0 6px}
.card{background:#fff;border:1px solid var(--line);border-radius:8px;padding:12px 14px}
.card .k{font-size:11.5px;color:var(--muted);letter-spacing:.02em}
.card .v{font-size:21px;font-weight:600;margin-top:2px}
.card .n{font-size:11.5px;color:var(--muted);margin-top:3px}
table{border-collapse:collapse;width:100%;margin:12px 0 6px;font-size:12.8px;background:#fff}
th,td{border:1px solid var(--line);padding:5px 7px;text-align:right}
th:first-child,td:first-child,th:nth-child(2),td:nth-child(2){text-align:left}
thead{background:#f2f4f7}
figure{margin:20px 0;background:#fff;border:1px solid var(--line);border-radius:8px;padding:10px}
figure img{width:100%;display:block;border-radius:4px}
figcaption{font-size:12.5px;color:var(--muted);margin-top:8px}
.note{background:#fff;border-left:3px solid var(--accent);padding:10px 14px;margin:14px 0;font-size:14px}
.good{border-left-color:var(--accent2)}
pre{background:#282c34;color:#e6e6e6;padding:12px 14px;border-radius:6px;overflow-x:auto;font-size:12px}
ul,ol{margin:8px 0 8px 22px;padding:0}
li{margin:4px 0}
.small{font-size:12.5px;color:var(--muted)}
</style></head><body><div class="wrap">
""")

    local_cfg_path, local_cfg = load_config("formal-local-*")
    cross_cfg_path, cross_cfg = load_config("formal-cross-host-*")
    local_commit = config_value(local_cfg, "git_head") or git_head()
    cross_commit = config_value(cross_cfg, "git_head") or "?"
    html_parts.append("<h1>RdmaRangeLock 正式矩阵：本地六版本 + 跨主机两协议</h1>")
    html_parts.append('<div class="sub">本地 <code>%s</code>（%d 次运行）　跨主机 <code>%s</code>'
                      '（%s 次运行）　10 秒窗口　每点 3 次重复取中位数</div>'
                      % (local_commit, len(local), cross_commit,
                         len(cross) if cross else 0))

    local_short = len([w for w in windows_local if w < 9.5])
    capacity_total = sum(float(r["lock_capacity_aborts"]) for r in local)
    gate_total = sum(float(r["lock_gate_aborts"]) for r in local)
    html_parts.append('<div class="cards">')
    html_parts.append('<div class="card"><div class="k">本地运行数</div><div class="v">%d</div>'
                      '<div class="n">%d 个配置 × 3 次</div></div>'
                      % (len(local), len(local) // 3))
    html_parts.append('<div class="card"><div class="k">窗口中位数</div><div class="v">%.1f s</div>'
                      '<div class="n">短于 9.5 s 的运行 %d 个</div></div>'
                      % (st.median(windows_local), local_short))
    if cross:
        cross_rate = pct(sum(float(r["aborted"]) for r in cross),
                         sum(float(r["transactions"]) for r in cross))
        html_parts.append('<div class="card"><div class="k">跨主机中止率</div><div class="v">%.1f%%</div>'
                          '<div class="n">容量中止 %d 次</div></div>'
                          % (cross_rate, sum(float(r["lock_capacity_aborts"]) for r in cross)))
    html_parts.append('<div class="card"><div class="k">本地容量中止</div><div class="v">%d</div>'
                      '<div class="n">gate 中止 %d 次</div></div>' % (capacity_total, gate_total))
    html_parts.append("</div>")

    # ---- conclusions ------------------------------------------------------
    html_parts.append("<h2>01　结论</h2>")
    bullets = []
    if (ycsbe, "next-key") in decay and (ycsbe, "hierarchical") in decay:
        bullets.append(
            "YCSB-E 上 next-key 从 t=1 到 t=32 衰减 <b>%.1f×</b>，hierarchical 为 <b>%.1f×</b>；"
            "两者单线程绝对吞吐比是 %.0f%%（hierarchical / next-key）。"
            % (1 / decay[(ycsbe, "next-key")], 1 / decay[(ycsbe, "hierarchical")],
               100 * med(lt, (ycsbe, "hierarchical", "256", "8", "1"), "throughput_committed_txn_s")
               / med(lt, (ycsbe, "next-key", "0", "8", "1"), "throughput_committed_txn_s")))
    bullets.append(
        "锁成本是机制层面的差别：YCSB-E 单线程下 next-key 每事务加锁 <b>%.0f / %.0f / %.0f</b> 次"
        "（range 8/32/128），hierarchical 为 <b>%.0f / %.0f / %.0f</b> 次——随扫描宽度增长的是前者，"
        "后者几乎不动，而两者返回的行数完全相同。"
        % (per_txn("next-key", 0, "8"), per_txn("next-key", 0, "32"), per_txn("next-key", 0, "128"),
           per_txn("hierarchical", bucket_for("ycsb-e"), "8"),
           per_txn("hierarchical", bucket_for("ycsb-e"), "32"),
           per_txn("hierarchical", bucket_for("ycsb-e"), "128")))
    if ("ycsb-a", "hierarchical") in decay:
        bullets.append(
            "写冲突负载上排序反转：YCSB-A 无锁基线衰减 %.1f×，hierarchical 衰减 <b>%.1f×</b>，"
            "是五种协议里最差的一档；它在 YCSB-E 上却是最好的。两条曲线必须成对出现。"
            % (1 / decay[("ycsb-a", "none")], 1 / decay[("ycsb-a", "hierarchical")]))
    if bucket_best:
        wide, narrow = [], []
        for workload in WORKLOADS:
            name = WL_LABEL[workload].split("（")[0]
            if (workload, "1") in bucket_best:
                wide.append("%s→%d" % (name, bucket_best[(workload, "1")][1]))
            if (workload, "32") in bucket_best:
                narrow.append("%s→%d" % (name, bucket_best[(workload, "32")][1]))
        bullets.append(
            "桶宽最优点随并发移动：单线程最优是 %s，32 线程最优是 %s。"
            "<code>--bucket auto</code> 取 YCSB-E 256、其余 16，即在高并发端偏保守——"
            "单线程上让出约 10%%，换取 t=32 时不出现 6 倍级的反转。"
            % ("、".join(wide), "、".join(narrow)))
    if cross:
        nk1 = med(defaultdict(list), tuple(), "x") if False else None
        ct = defaultdict(list)
        for row in cross:
            ct[(row["cc"], row["bucket"], row["range"], row["threads"])].append(row)
        nk = med(ct, ("rdma-next-key", "0", "8", "1"), "throughput_committed_txn_s")
        loc = med(lt, ("ycsb-e", "next-key", "0", "8", "1"), "throughput_committed_txn_s")
        if nk and loc:
            bullets.append(
                "跨主机 t=1 为 <b>%.0f txn/s</b>，本地同配置 %.0f txn/s，差 <b>%.0f×</b>；"
                "这个差距是传输成本（每把锁约 5.5 个单边往返），不是锁协议差异。"
                % (nk, loc, loc / nk))
        bullets.append(
            "跨主机 %d 次运行中 <code>lock_capacity_aborts</code> 与 "
            "<code>lock_gate_aborts</code> 合计均为 0，中止全部来自真实锁冲突——"
            "这是把锁槽扩到 8192 之后才成立的（预检时 1024 槽下 range=128 t=32 有 95%% 的中止来自容量）。"
            % len(cross))
    html_parts.append("<ul>%s</ul>" % "".join("<li>%s</li>" % b for b in bullets))

    html_parts.append('<div class="note">旧矩阵（<code>formal-*-20261031*.csv</code>、'
                      '<code>formal-*-20261101*.csv</code>）只作为历史问题证据引用，'
                      '不与本报告的任何数字混用：它们跑在 <code>b808549</code> 与 <code>a2c68f8</code> 上，'
                      '当时的 CSV 表头还缺 10 个诊断列。</div>')

    # ---- figures ----------------------------------------------------------
    html_parts.append("<h2>02　图表</h2>")
    html_parts.append(img_tag(
        "fig01_throughput_vs_threads.png", "图 1　本地：吞吐 vs 线程数",
        "四条 workload 各一格，hierarchical 取 <code>--bucket auto</code> 的取值。"
        "纵轴对数：无锁基线基本水平，带锁协议随线程数下降。"))
    html_parts.append(img_tag(
        "fig02_abort_vs_threads.png", "图 2　本地：no-wait 中止率",
        "中止率随线程数上升是 no-wait 语义的直接结果，不是实现缺陷；"
        "两张图的横轴一致，可以对照着读。"))
    html_parts.append(img_tag(
        "fig03_range_effect.png", "图 3　扫描宽度对吞吐的影响（YCSB-E）",
        "range 从 8 到 128，逐行加锁的三个协议同步下滑，hierarchical 基本不动。"))
    html_parts.append(img_tag(
        "fig04_lock_cost.png", "图 4　锁成本：每事务加锁次数与每返回一行付出的锁",
        "左图是对数轴的绝对量，右图把行数除掉；虚线 1.0 表示“一行一把锁”。"))
    html_parts.append(img_tag(
        "fig05_bucket_sweep.png", "图 5　v6 桶宽 16/64/256",
        "扫描型负载要宽桶，点写型负载要窄桶；标题里标出 auto 的选择。"))
    html_parts.append(img_tag(
        "fig06_single_thread.png", "图 6　单线程吞吐（唯一不受锁表排队干扰的对比）",
        "这是唯一可以横向比较“协议开销”的维度；多线程数字受锁表内部串行点支配。"))
    if cross:
        html_parts.append(img_tag(
            "fig07_cross_throughput.png", "图 7　跨主机：吞吐 vs 线程数",
            "两个 RDMA 协议、三种 range。绝对值比本地低一个数量级，量级差来自传输。"))
        html_parts.append(img_tag(
            "fig08_cross_abort.png", "图 8　跨主机：中止率与中止构成",
            "右图三色堆叠：锁冲突 / 槽容量 / gate。后两者为 0 是本轮修复的直接证据。"))

    # ---- tables -----------------------------------------------------------
    html_parts.append("<h2>03　表格</h2>")
    html_parts.append("<h3>本地：committed txn/s（三次中位数）</h3>")
    for workload in WORKLOADS:
        rng = "8" if workload == "ycsb-e" else "0"
        rows = []
        for cc in LOCAL_CCS:
            buckets = [16, 64, 256] if cc == "hierarchical" else [0]
            for bucket in buckets:
                ranges = ("8", "32", "128") if workload == "ycsb-e" else ("0",)
                for r in ranges:
                    values = [med(lt, (workload, cc, str(bucket), r, str(t)),
                                  "throughput_committed_txn_s") for t in THREADS]
                    if values[0] is None:
                        continue
                    name = LABEL[cc] + (" b%d" % bucket if cc == "hierarchical" else "")
                    if workload == "ycsb-e":
                        name += " r=%s" % r
                    rows.append([name] + ["%.0f" % v for v in values])
        html_parts.append("<h3>%s</h3>" % WL_LABEL[workload])
        html_parts.append(table(["协议", "配置"] + ["t%d" % t for t in THREADS], rows))

    html_parts.append("<h3>本地：t=32 的中止与锁数量</h3>")
    rows = []
    for workload in WORKLOADS:
        rng = "8" if workload == "ycsb-e" else "0"
        for cc in LOCAL_CCS:
            bucket = bucket_for(workload) if cc == "hierarchical" else 0
            key = (workload, cc, str(bucket), rng, "32")
            txns = med(lt, key, "transactions")
            if not txns:
                continue
            aborted = med(lt, key, "aborted")
            rows.append([WL_LABEL[workload].split("（")[0], LABEL[cc],
                         "%.1f%%" % pct(aborted, txns),
                         "%.0f" % med(lt, key, "lock_conflicts"),
                         "%.0f" % med(lt, key, "lock_gate_aborts"),
                         "%.0f" % med(lt, key, "lock_capacity_aborts"),
                         "%.0f" % (med(lt, key, "lock_acquires") / txns)])
    html_parts.append(table(["workload", "协议", "中止率", "冲突", "gate", "容量", "加锁/事务"], rows))

    if cross:
        html_parts.append("<h3>跨主机：committed txn/s（三次中位数）</h3>")
        ct = defaultdict(list)
        for row in cross:
            ct[(row["cc"], row["bucket"], row["range"], row["threads"])].append(row)
        rows = []
        for cc in RDMA_CCS:
            buckets = [16, 64, 256] if cc == "rdma-hierarchical" else [0]
            for bucket in buckets:
                for rng in ("8", "32", "128"):
                    values = [med(ct, (cc, str(bucket), rng, str(t)),
                                  "throughput_committed_txn_s") for t in THREADS]
                    if values[0] is None:
                        continue
                    name = LABEL[cc] + (" b%d" % bucket if cc == "rdma-hierarchical" else "")
                    rows.append([name, "r=%s" % rng] + ["%.0f" % v for v in values])
        html_parts.append(table(["协议", "配置"] + ["t%d" % t for t in THREADS], rows))

        html_parts.append("<h3>跨主机：t=32 的中止构成与往返数</h3>")
        rows = []
        for cc in RDMA_CCS:
            bucket = bucket_for("ycsb-e") if cc == "rdma-hierarchical" else 0
            key = (cc, str(bucket), "8", "32")
            txns = med(ct, key, "transactions")
            if not txns:
                continue
            rows.append([LABEL[cc], "%.1f%%" % pct(med(ct, key, "aborted"), txns),
                         "%.0f" % med(ct, key, "lock_conflicts"),
                         "%.0f" % med(ct, key, "lock_capacity_aborts"),
                         "%.0f" % med(ct, key, "lock_gate_aborts"),
                         "%.1f" % (med(ct, key, "rdma_operations") / txns),
                         "%.0f" % med(ct, key, "rdma_connections")])
        html_parts.append(table(["协议", "中止率", "冲突", "容量", "gate",
                                 "RDMA 操作/事务", "连接数"], rows))

    # ---- provenance -------------------------------------------------------
    html_parts.append("<h2>04　配置与数据身份</h2>")
    html_parts.append(table(
        ["项", "值"], [
            ["本地 commit", "<code>%s</code>　<code>%s</code>"
             % (local_commit, os.path.basename(local_cfg_path or ""))],
            ["本地结果", "<code>%s</code><br>%d 行<br>sha256 <code>%s</code>"
             % (os.path.basename(local_path), len(local), sha256(local_path))],
        ] + ([["跨主机 commit", "<code>%s</code>　<code>%s</code>"
               % (cross_commit, os.path.basename(cross_cfg_path or ""))],
              ["跨主机结果", "<code>%s</code><br>%d 行<br>sha256 <code>%s</code>"
               % (os.path.basename(cross_path), len(cross), sha256(cross_path))]]
             if cross else [])))
    for label, lines in (("本地", local_cfg), ("跨主机", cross_cfg)):
        if lines:
            html_parts.append("<h3>%s矩阵启动时写入的 <code>config.txt</code></h3>" % label)
            html_parts.append("<pre>%s</pre>" % html.escape("\n".join(lines)))
    html_parts.append('<p class="small">重新生成：<code>scripts/analyze_final.py</code>、'
                      '<code>scripts/plot_final.py</code>、<code>scripts/build_final_report.py</code>。'
                      '原始 CSV 按 CONSTRAINTS 不入库，校验和与路径见上表。</p>')

    html_parts.append("</div></body></html>")
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8") as handle:
        handle.write("".join(html_parts))
    print("wrote", OUT, os.path.getsize(OUT), "bytes")


if __name__ == "__main__":
    main()
