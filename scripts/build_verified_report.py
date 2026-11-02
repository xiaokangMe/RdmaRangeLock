#!/usr/bin/env python3
"""Validate explicit campaign inputs and generate figures, tables, MD and offline HTML."""
import argparse
import base64
import collections
import csv
import hashlib
import html
import itertools
import json
import math
from pathlib import Path
import statistics as st

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

ROOT = Path(__file__).resolve().parent.parent
ERRORS = ["lock_errors", "index_errors", "lock_release_failures", "failed_transactions",
          "rdma_timeouts", "rdma_completion_errors", "rdma_count_errors", "rdma_local_errors"]
COLORS = {"none":"#777777","record-2pl":"#2274a5","gap-lock":"#ed9b40",
          "next-key":"#23956f","hierarchical":"#bf4e70",
          "rdma-next-key":"#23956f","rdma-hierarchical":"#bf4e70"}
THREADS = [1,2,4,8,16,32]
WORKLOADS = ["ycsb-a","ycsb-e","tpcc-payment","tpcc-new-order"]
LABEL = {"none":"no CC", "record-2pl":"record 2PL", "gap-lock":"gap lock",
         "next-key":"next-key", "hierarchical":"hierarchy",
         "rdma-next-key":"RDMA next-key", "rdma-hierarchical":"RDMA hierarchy"}


def read(path):
    with Path(path).open(newline="") as handle:
        reader=csv.DictReader(handle)
        rows=list(reader)
    if len(reader.fieldnames) != len(set(reader.fieldnames)) or any(None in r or None in r.values() for r in rows):
        raise ValueError("CSV schema mismatch: "+str(path))
    return rows


def key(row):
    return (row["workload"], int(row["range"]), row["cc"], int(row["bucket"]), int(row["threads"]))


def metric(row, name):
    if name=="abort_pct": return 100*int(row["aborted"])/max(1,int(row["transactions"]))
    if name=="lock_per_attempt": return int(row["lock_acquires"])/max(1,int(row["transactions"]))
    if name=="wr_per_attempt": return int(row["rdma_operations"])/max(1,int(row["transactions"]))
    if name=="recheck_per_attempt": return int(row["rows_rechecked"])/max(1,int(row["transactions"]))
    return float(row[name])


def span(rows, field, digits=1):
    values=[metric(r,field) for r in rows]
    return f"{st.median(values):,.{digits}f} [{min(values):,.{digits}f}, {max(values):,.{digits}f}]"


def valid(row):
    return int(row["elapsed_ns"]) >= 10_000_000_000 and not any(int(row[k]) for k in ERRORS)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--campaign",required=True)
    parser.add_argument("--stem",default="formal-verified-2026-11-02")
    args=parser.parse_args()
    manifest=json.loads(Path(args.campaign).read_text())
    if manifest["status"]!="complete": raise ValueError("campaign is not complete")
    config=manifest["config"]
    raw={m:read(manifest[m+"_csv"]) for m in ["local","cross-host"]}
    groups={m:collections.defaultdict(list) for m in raw}
    audits={}
    for mode,rows in raw.items():
        expected=set()
        for wl in config["workloads"]:
            for rng in (config["ranges"] if wl=="ycsb-e" else [0]):
                for cc in config["local_ccs" if mode=="local" else "rdma_ccs"]:
                    for bucket in (config["buckets"] if cc.endswith("hierarchical") else [0]):
                        for thread in config["threads"]:
                            for run in range(1,config["runs"]+1):
                                expected.add((wl,rng,cc,bucket,thread,run,7+run))
        observed=[]
        for row in rows:
            observed.append((*key(row),int(row["run"]),int(row["seed"])))
            groups[mode][key(row)].append(row)
            if int(row["committed"])+int(row["aborted"]) != int(row["transactions"]):
                raise ValueError("bad transaction accounting")
            if not valid(row): raise ValueError("invalid time/error row")
            if int(row["rdma_profile_enabled"]): raise ValueError("profile enabled")
        if len(observed)!=len(set(observed)) or set(observed)!=expected:
            raise ValueError("missing/duplicate/unexpected cases: "+mode)
        if any(len(g)!=3 for g in groups[mode].values()):
            raise ValueError("not exactly 3 repeats")
        sha=hashlib.sha256(Path(manifest[mode+"_csv"]).read_bytes()).hexdigest()
        if sha != manifest[mode+"_sha256"]: raise ValueError("raw CSV changed")
        audits[mode]=dict(cases=len(rows),groups=len(groups[mode]),
            min_seconds=min(int(r["elapsed_ns"])/1e9 for r in rows),
            max_seconds=max(int(r["elapsed_ns"])/1e9 for r in rows),
            errors={f:sum(int(r[f]) for r in rows) for f in ERRORS},
            capacity=sum(int(r["lock_capacity_aborts"]) for r in rows),
            gate=sum(int(r["lock_gate_aborts"]) for r in rows),sha256=sha)

    figdir=ROOT/"figures"/args.stem
    figdir.mkdir(parents=True,exist_ok=True)
    reportdir=ROOT/"docs/reports"
    reportdir.mkdir(parents=True,exist_ok=True)
    notedir=ROOT/"docs/notes"
    resultdir=ROOT/"experiments/results"
    plt.rcParams.update({"font.size":9,"axes.spines.top":False,"axes.spines.right":False,
                         "axes.grid":True,"grid.alpha":0.23,"legend.frameon":False})
    figures=[]

    def save(fig,name,caption):
        fig.tight_layout()
        p=figdir/(name+".png")
        fig.savefig(p,dpi=160,bbox_inches="tight")
        plt.close(fig)
        figures.append((p,caption))
        return p

    def series(ax,mode,wl,rng,cc,bucket,field,label=None):
        g=[groups[mode][(wl,rng,cc,bucket,t)] for t in THREADS]
        data=[[metric(r,field) for r in rows] for rows in g]
        med=[st.median(v) for v in data]
        color=COLORS[cc]
        if "hierarchical" in cc:
            color={16:"#db6c79",64:"#aa3b71",256:"#622d82"}[bucket]
        ax.plot(THREADS,med,marker="o",lw=1.5,ms=3,color=color,label=label or LABEL[cc])
        ax.fill_between(THREADS,[min(v) for v in data],[max(v) for v in data],color=color,alpha=.12)
        ax.set_xscale("log",base=2)
        ax.set_xticks(THREADS,labels=[str(t) for t in THREADS])
        return med

    for mode,title in [("local","Local (.66)"),("cross-host","Cross-host (.67 client / .66 server)")]:
        for field,name,ylabel in [("throughput_committed_txn_s","throughput","committed txn/s"),
                                  ("abort_pct","abort","aborted attempts (%)")]:
            fig,axes=plt.subplots(2,2,figsize=(12,8))
            for ax,wl in zip(axes.flat,WORKLOADS):
                rng=8 if wl=="ycsb-e" else 0
                ccs=config["local_ccs" if mode=="local" else "rdma_ccs"]
                for cc in ccs:
                    for bucket in (config["buckets"] if cc.endswith("hierarchical") else [0]):
                        label=LABEL[cc]+(f" b{bucket}" if bucket else "")
                        series(ax,mode,wl,rng,cc,bucket,field,label)
                ax.set_title(wl+(" (range=8)" if rng else ""))
                ax.set_xlabel("threads")
                ax.set_ylabel(ylabel)
                if field=="abort_pct": ax.set_ylim(0,100)
                else: ax.set_yscale("log")
            axes.flat[0].legend(fontsize=7,ncol=2)
            fig.suptitle(title+": "+name+"; median and min/max of 3, >=10 s per run",fontsize=12)
            save(fig,mode+"-"+name,
                 ("本地" if mode=="local" else "跨主机")+"："+
                 ("成功事务吞吐" if field!="abort_pct" else "中止率")+
                 "，曲线为每点三次的中位数，阴影为最小—最大；层级锁三个 bucket 均展示。")

    for mode in raw:
        fig,axes=plt.subplots(1,3,figsize=(15,4.5))
        for ax,rng in zip(axes,[8,32,128]):
            for cc in config["local_ccs" if mode=="local" else "rdma_ccs"]:
                for b in (config["buckets"] if cc.endswith("hierarchical") else [0]):
                    series(ax,mode,"ycsb-e",rng,cc,b,"throughput_committed_txn_s",LABEL[cc]+(f" b{b}" if b else ""))
            ax.set_yscale("log")
            ax.set_title(f"YCSB-E range={rng}")
            ax.set_xlabel("threads"); ax.set_ylabel("committed txn/s")
        axes[0].legend(fontsize=7,ncol=2)
        fig.suptitle(mode+": range-length sensitivity",fontsize=12)
        save(fig,mode+"-range","YCSB-E 的 range=8/32/128 扩展性，避免只挑短范围结果。")

    fig,axes=plt.subplots(1,2,figsize=(13,4.8))
    for ax,mode in zip(axes,raw):
        for cc in config["local_ccs" if mode=="local" else "rdma_ccs"]:
            if cc=="none": continue
            for b in (config["buckets"] if cc.endswith("hierarchical") else [0]):
                data=[groups[mode][("ycsb-e",r,cc,b,1)] for r in [8,32,128]]
                v=[[metric(x,"lock_per_attempt") for x in g] for g in data]
                color=COLORS[cc]
                ax.plot([8,32,128],[st.median(x) for x in v],marker="o",label=LABEL[cc]+(f" b{b}" if b else ""))
        ax.set_yscale("log"); ax.set_xticks([8,32,128])
        ax.set_xlabel("range length"); ax.set_ylabel("lock() attempts / txn attempt")
        ax.set_title(mode+", t=1"); ax.legend(fontsize=7,ncol=2)
    save(fig,"lock-amplification","锁请求次数/尝试事务，t=1；这是调用强度，不等于峰值同时持锁数。")

    fig,axes=plt.subplots(1,3,figsize=(15,4.7))
    for ax,rng in zip(axes,[8,32,128]):
        for cc in config["rdma_ccs"]:
            for b in (config["buckets"] if cc.endswith("hierarchical") else [0]):
                series(ax,"cross-host","ycsb-e",rng,cc,b,"wr_per_attempt",LABEL[cc]+(f" b{b}" if b else ""))
        ax.set_yscale("log"); ax.set_xlabel("threads"); ax.set_ylabel("submitted WR / txn attempt")
        ax.set_title("range="+str(rng))
    axes[0].legend(fontsize=7)
    save(fig,"rdma-wr-amplification","RDMA WR/尝试事务；profile 关闭时没有同步提交与排队计时，不能把 WR 数当 RTT。")

    fig,axes=plt.subplots(1,2,figsize=(13,4.8))
    for ax,mode in zip(axes,raw):
        vals=[int(r["elapsed_ns"])/1e9 for r in raw[mode]]
        ax.hist(vals,bins=30,color="#2274a5",alpha=.8)
        ax.axvline(10,color="#bf4e70",ls="--")
        ax.set_title(f"{mode}, n={len(vals)}, min={min(vals):.5f}s")
        ax.set_xlabel("measured wall time (s)"); ax.set_ylabel("cases")
    save(fig,"measurement-windows","所有 case 的真实 elapsed 分布；10 秒结束后包含完成/中止当前事务与释放锁的时间。")

    # Aggregate per-run ratios before taking median; never ratio of unrelated medians.
    rows_summary=[]
    unstable=[]
    for mode,table in groups.items():
        for k,rows in sorted(table.items()):
            values=[float(r["throughput_committed_txn_s"]) for r in rows]
            spread=(max(values)-min(values))/max(st.median(values),1e-12)
            out=dict(mode=mode,workload=k[0],range=k[1],cc=k[2],bucket=k[3],threads=k[4],
                     samples=len(rows),throughput_median=st.median(values),
                     throughput_min=min(values),throughput_max=max(values),
                     spread_pct=spread*100,
                     abort_pct_median=st.median(metric(r,"abort_pct") for r in rows),
                     lock_per_attempt_median=st.median(metric(r,"lock_per_attempt") for r in rows),
                     wr_per_attempt_median=st.median(metric(r,"wr_per_attempt") for r in rows))
            rows_summary.append(out)
            if spread>.30: unstable.append(out)
    summarypath=resultdir/(args.stem+"-summary.csv")
    with summarypath.open("w",newline="") as h:
        w=csv.DictWriter(h,fieldnames=list(rows_summary[0])); w.writeheader(); w.writerows(rows_summary)
    (resultdir/(args.stem+"-audit.json")).write_text(json.dumps(
        dict(source_commit=manifest["source_commit"],audits=audits,unstable_groups=unstable),indent=2))
    fig,axes=plt.subplots(1,2,figsize=(13,4.7))
    for ax,mode in zip(axes,raw):
        ax.hist([r["spread_pct"] for r in rows_summary if r["mode"]==mode],bins=30,color="#ed9b40")
        ax.axvline(30,color="#bf4e70",ls="--"); ax.set_title(mode)
        ax.set_xlabel("(max - min) / median throughput (%)"); ax.set_ylabel("parameter groups")
    save(fig,"repeat-spread","重复点波动；seed 也随 run 改变，三次不是纯计时噪声重复，不能据此构造严谨置信区间。")

    def table_markdown(mode,wl,rng,field):
        lines=["| 协议 / bucket | "+" | ".join("t"+str(t) for t in THREADS)+" |",
               "|---|"+"---:|"*len(THREADS)]
        for cc in config["local_ccs" if mode=="local" else "rdma_ccs"]:
            for b in (config["buckets"] if cc.endswith("hierarchical") else [0]):
                label=cc+(f" / {b}" if b else "")
                lines.append("| "+label+" | "+" | ".join(span(groups[mode][(wl,rng,cc,b,t)],field) for t in THREADS)+" |")
        return "\n".join(lines)

    findings=[]
    for mode in raw:
        g=groups[mode]
        if mode=="local":
            first=g[("ycsb-a",0,"none",0,1)]
            last=g[("ycsb-a",0,"none",0,32)]
            a=st.median(metric(r,"throughput_committed_txn_s") for r in first)
            b=st.median(metric(r,"throughput_committed_txn_s") for r in last)
            findings.append(f"本地 YCSB-A 的 none 基线：t1={a:,.0f}、t32={b:,.0f} txn/s（{b/a:.3f}×）；该路径中止为零，扩展性下降不能归因于事务锁中止，需单独定位索引/共享资源争用。")
        for cc in config["local_ccs" if mode=="local" else "rdma_ccs"]:
            if cc=="none": continue
            b=16 if cc.endswith("hierarchical") else 0
            rs=g[("ycsb-a",0,cc,b,32)]
            pct=st.median(metric(r,"abort_pct") for r in rs)
            throughput=st.median(metric(r,"throughput_committed_txn_s") for r in rs)
            findings.append(f"{mode} / YCSB-A / {cc}"+(f" b{b}" if b else "")+
                            f" / t32：成功吞吐 {throughput:,.1f} txn/s，中止率 {pct:.2f}%；热点 no-wait 结果要同时看提交与尝试吞吐，不能仅用 attempts 衡量收益。")
    issues=[]
    for mode,a in audits.items():
        if a["capacity"]: issues.append(f"{mode} 容量中止 {a['capacity']} 次：这些点测到槽容量限制，不能归为纯锁冲突。")
        if a["gate"]: issues.append(f"{mode} gate 中止 {a['gate']} 次：与逻辑冲突分开统计。")
    if unstable:
        issues.append(f"{len(unstable)}/{len(rows_summary)} 个参数组的 (max−min)/median >30%；应先复测这些点，不能只取最好一次。")
    issues += [
        "优先定位本地无事务锁的索引扩展性：分别测 update、insert/delete，加入根/结构写路径与 latch 等待的定向采样；本矩阵没有这些计时，不能断定某一把 latch 是唯一原因。",
        "再分析热点下 no-wait 的中止与浪费：固定 Zipf、bucket、线程做单变量对照，保留原隔离和提交后释放时机，不通过增添 sleep 掩盖冲突。",
        "前一轮固定 5,000 事务检查误用了脚本原默认 duration=0；这批 756+432 行未用于本报告正式图表。脚本默认已改为 10，正式结果逐行检查 >=10 秒。",
        "当前 next-key 仍逐区间调用 lock；本轮没有实现跨逻辑区间的批处理。WR/事务高只能说明操作量，profiling=0 时无法从这些数据定量区分软件排队、gate 等待与 NIC 往返。",
        "现有曲线同时改变协议和锁覆盖粒度，hierarchical 的较少调用与更粗冲突域需要一起解释；bucket 16/64/256 全部保留，没有每点挑选最佳 bucket。",
        "本地工作负载在 .66、跨主机工作负载在 .67；本地/远端吞吐比包含客户端平台和传输路径，不能称为纯网络延迟或 RDMA 加速比。",
        "none 是无事务隔离的数据路径参照，record-2pl 不防幻读；协议性能排序不能代替隔离正确性结论。",
        "三个 run 用 seed 8/9/10；按 run 配对可比较同 trace，但三次的最小—最大不等同固定 trace 的纯运行波动，也不是置信区间。",
        "TPC-C 仅覆盖本项目的 Payment/New-Order 子负载与键映射，不是完整标准 TPC-C 吞吐认证，不能标为 tpmC。",
        "这些矩阵不替代一小时结构/事务正确性压力；十秒收尾发生的事务中止仍进入计数，不应把 aborted 全部简单归为 conflict。"
    ]
    lines=[
        "# 新代码正式矩阵报告（2026-11-02）","",
        f"测量源码提交：{manifest['source_commit']}；C++ 实现基线为 596c94f，此后测量提交只增加运行配置与验收脚本。",
        f"配置：experiments/configs/formal_both_10s_20261102.json；原始 manifest：{Path(args.campaign).name}。",
        f"运行开始 {manifest['started']}；结束 {manifest['finished']}（.66 主机本地时钟）。",
        "两端同源码 Release；mlx5_0 / GID=3；.66=10.0.0.3 server，.67=10.0.0.4 client；本地矩阵在 .66。",
        "每 case >=10 秒，三次，seed=8/9/10；10,000 keys，trace 5,000 transactions 循环，10 ops/txn，Zipf=0.99，TPC-C items=5。",
        "profile=0；linear；local gate=1；gate retry=64；RDMA QP=min(threads,2)。范围 8/32/128，线程 1/2/4/8/16/32，bucket 16/64/256。",
        "", "## 数据验收","",
        "| 矩阵 | case | 参数组 | elapsed 最短/最长（秒） | 硬错误 | 容量中止 | gate 中止 |",
        "|---|---:|---:|---:|---:|---:|---:|"
    ]
    for mode,a in audits.items():
        lines.append(f"| {mode} | {a['cases']} | {a['groups']} | {a['min_seconds']:.5f} / {a['max_seconds']:.5f} | {sum(a['errors'].values())} | {a['capacity']} | {a['gate']} |")
    lines += ["","全部预期参数组合、run/seed、唯一性、三次重复、CSV 列宽、事务守恒及 profile 关闭均通过检查。",
              "硬件 CTest 24/24、跨主机定向 3/3、16 点十秒容量预检先于正式矩阵完成；实验顺序执行，不并行争用 .66。",
              "","## 图表与解释",""]
    for p,caption in figures:
        lines += [f"![{caption}](../../figures/{args.stem}/{p.name})",caption,""]
    lines += ["## 主要测量发现",""]+findings+["","## 成功吞吐数表","","所有单元格为中位数 [最小值, 最大值]，单位 committed txn/s。"]
    for mode in raw:
        for wl in WORKLOADS:
            for rng in ([8,32,128] if wl=="ycsb-e" else [0]):
                lines += ["",f"### {mode} / {wl} / range={rng}","",table_markdown(mode,wl,rng,"throughput_committed_txn_s")]
    lines += ["","## 中止构成与测量边界","",
              "| 矩阵 / 协议 | 尝试事务 | 中止 | conflict | capacity | gate | 其余中止 |",
              "|---|---:|---:|---:|---:|---:|---:|"]
    for mode in raw:
        for cc in config["local_ccs" if mode=="local" else "rdma_ccs"]:
            rs=[r for r in raw[mode] if r["cc"]==cc]
            totals={f:sum(int(r[f]) for r in rs) for f in
                    ["transactions","aborted","lock_conflicts","lock_capacity_aborts","lock_gate_aborts"]}
            other=totals["aborted"]-totals["lock_conflicts"]-totals["lock_capacity_aborts"]-totals["lock_gate_aborts"]
            lines.append("| "+mode+" / "+cc+" | "+" | ".join(f"{totals[f]:,}" for f in
                         ["transactions","aborted","lock_conflicts","lock_capacity_aborts","lock_gate_aborts"])+f" | {other:,} |")
    lines += ["其余中止包括窗口结束、扫描后验证等没有独立列的路径，不能全部解释成逻辑锁冲突。总数只作核账；不同点尝试次数差异很大，性能比较使用逐点比率。",
              "","## 发现的问题与建议顺序",""]

    lines += [f"{i}. {text}" for i,text in enumerate(issues,1)]
    lines += ["","## 复现和数据定位","",
        f"本地 CSV：{manifest['local_csv']}",
        f"跨主机 CSV：{manifest['cross-host_csv']}",
        f"本地 SHA-256：{audits['local']['sha256']}",
        f"跨主机 SHA-256：{audits['cross-host']['sha256']}",
        f"完整日志：{manifest['log_dir']}",
        f"远端快照：{manifest['remote_root']}",
        "运行：python3 scripts/run_verified_matrices.py",
        f"报告：python3 scripts/build_verified_report.py --campaign {args.campaign}",
        "原始 CSV/JSON、日志、源码归档与构建产物不入 Git；图表、分析脚本与报告入库。"
    ]
    normalized=[]
    i=0
    while i<len(lines):
        if lines[i].startswith("|") and "\n" not in lines[i]:
            block=[]
            while i<len(lines) and lines[i].startswith("|") and "\n" not in lines[i]:
                block.append(lines[i]); i+=1
            normalized.append("\n".join(block))
        else:
            normalized.append(lines[i]); i+=1
    md="\n\n".join(normalized)+"\n"
    (notedir/(args.stem+".md")).write_text(md)
    parts=[]
    for line in normalized:
        if line.startswith("!["): continue
        if line.startswith("# "): parts.append("<h1>"+html.escape(line[2:])+"</h1>")
        elif line.startswith("## "): parts.append("<h2>"+html.escape(line[3:])+"</h2>")
        elif line.startswith("### "): parts.append("<h3>"+html.escape(line[4:])+"</h3>")
        elif line.startswith("| "):
            if len(line.splitlines())>1 and line.splitlines()[1].startswith("|---"):
                table=[]
                for n,l in enumerate(line.splitlines()):
                    if n==1: continue
                    tag="th" if n==0 else "td"
                    table.append("<tr>"+"".join(f"<{tag}>"+html.escape(c.strip())+f"</{tag}>" for c in l.strip("|").split("|"))+"</tr>")
                parts.append('<div class="table"><table>'+"".join(table)+"</table></div>")
            else: parts.append("<p>"+html.escape(line)+"</p>")
        elif line: parts.append("<p>"+html.escape(line)+"</p>")
    # Place images after matching captions. One self-contained artifact.
    body="\n".join(parts)
    for p,caption in figures:
        marker="<p>"+html.escape(caption)+"</p>"
        image='<figure><img alt="'+html.escape(caption)+'" src="data:image/png;base64,'+base64.b64encode(p.read_bytes()).decode()+'"><figcaption>'+html.escape(caption)+'</figcaption></figure>'
        body=body.replace(marker,image,1)
    css="""body{font-family:system-ui,'Microsoft YaHei',sans-serif;background:#f4f6f9;color:#182332;margin:0}
main{max-width:1300px;margin:auto;padding:36px;background:white}h1,h2,h3{color:#163a59}
p{line-height:1.7;overflow-wrap:anywhere}.table{overflow:auto;margin:20px 0}table{border-collapse:collapse;width:100%;font-size:13px}
th,td{padding:8px;border-bottom:1px solid #d9e0e8;text-align:right;white-space:nowrap}th:first-child,td:first-child{text-align:left}
th{background:#edf2f7}img{max-width:100%;height:auto}figure{margin:28px 0}figcaption{color:#526277;font-size:14px}
h2{border-top:2px solid #e0e6ec;padding-top:20px}"""
    htmlpath=reportdir/(args.stem+".html")
    htmlpath.write_text('<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>正式矩阵报告</title><style>'+css+"</style><main>"+body+"</main></html>")
    print(json.dumps(dict(html=str(htmlpath),md=str(notedir/(args.stem+".md")),figures=len(figures),audits=audits,unstable_groups=len(unstable)),ensure_ascii=False,indent=2))


if __name__=="__main__":
    main()
