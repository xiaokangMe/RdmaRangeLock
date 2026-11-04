#!/usr/bin/env python3
"""Explicit shared-data campaign report; refuses historical/smoke input by default."""
import argparse
import collections
import csv
import json
from pathlib import Path
import statistics
from run_shared_matrices import cases, ERRORS, validate_row, config_fingerprint, hierarchy_layout, hierarchy_behavior


def load_campaign(path, allow_smoke=False):
    state=json.loads(Path(path).read_text())
    if state.get("status")!="complete":raise ValueError("campaign is not complete")
    formal=state.get("kind")=="formal-shared-data"
    if not formal and not (allow_smoke and state.get("kind")=="smoke-not-formal"):
        raise ValueError("requires new shared-data formal results; --allow-smoke is only for report validation")
    if state.get("failed_cases") or state.get("error"):raise ValueError("campaign contains failures")
    if formal and (state["config"]["runs"]<3 or state["config"]["duration_seconds"]<10):
        raise ValueError("formal report requires >=3 repetitions and >=10 seconds")
    groups=collections.defaultdict(list)
    for mode,path in state["result_files"].items():
        expected={(w,length,cc,b,t,run+1,state["config"]["seeds"][run],policy if cc!="none" else "none")
                  for w,length,cc,b,t,run,policy in cases(state["config"],mode)}
        observed=set()
        with Path(path).open(newline="") as f:
            reader=csv.DictReader(f);rows=list(reader)
        if len(reader.fieldnames)!=len(set(reader.fieldnames)) or any(None in row or None in row.values() for row in rows):
            raise ValueError("CSV schema mismatch")
        for row in rows:
            import io
            buf=io.StringIO();writer=csv.DictWriter(buf,fieldnames=list(row));writer.writeheader();writer.writerow(row)
            validate_row(buf.getvalue(),state["config"]["duration_seconds"],mode=="cross-host",
                         state["config"].get("coordination_backend","tcp-control+rdma-slots") if mode=="cross-host" else None,
                         hierarchy_layout(state["config"],row["cc"]),hierarchy_behavior(state["config"],row["cc"]))
            if int(row.get("coroutines",1))!=state["config"].get("coroutines",1) or int(row.get("warmup_seconds",0))!=state["config"].get("warmup_seconds",0) or int(row.get("execution_stack_bytes",262144))!=state["config"].get("coroutine_stack_bytes",262144):
                raise ValueError("mixed runtime geometry/warmup")
            if state["config"].get("warmup_seconds",0) and int(row.get("warmup_elapsed_ns",0))<state["config"]["warmup_seconds"]*1000000000:
                raise ValueError("warmup window too short")
            fingerprint=row.get("campaign_config_sha256")
            if fingerprint and fingerprint!=config_fingerprint(state["config"]):
                raise ValueError("CSV configuration fingerprint differs from campaign")
            if formal and state["config"].get("coordination_backend")=="native-rdma-state" and not fingerprint:
                raise ValueError("native formal result lacks configuration fingerprint")
            if row["result_kind"]!=state["kind"] or row["source_commit"]!=state["source_commit"]:
                raise ValueError("mixed result kind or code baseline")
            if any(int(row[k]) for k in ERRORS):raise ValueError("hard errors in CSV")
            if int(row["elapsed_ns"])<state["config"]["duration_seconds"]*1000000000:
                raise ValueError("measurement window too short")
            if mode=="cross-host" and (row["index"]!="rdma" or row["data_backend"]!="shared-rdma" or
                                      (int(row["committed"])>0 and int(row["data_rdma_operations"])<=0) or int(row["lock_capacity_aborts"]) or int(row["lock_gate_aborts"])):
                raise ValueError("wrong data backend or capacity/gate saturation")
            if mode=="local" and row["data_backend"]!="local":raise ValueError("wrong local backend")
            policy=row.get("wait_policy") if row["cc"]!="none" else "none"
            key=(row["workload"],int(row["range"]),row["cc"],int(row["bucket"]),int(row["threads"]),int(row["run"]),int(row["seed"]),policy)
            if key in observed:raise ValueError("duplicate case")
            observed.add(key)
            groups[(mode,*key[:5],policy)].append(row)
        if observed!=expected:raise ValueError("matrix dimensions are incomplete/unexpected")
    return state,groups


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--campaign",required=True)
    parser.add_argument("--out",required=True)
    parser.add_argument("--allow-smoke",action="store_true")
    args=parser.parse_args()
    state,groups=load_campaign(args.campaign,args.allow_smoke)
    out=Path(args.out);out.mkdir(parents=True,exist_ok=True)
    summary=[]
    for key,rows in sorted(groups.items()):
        mode,w,length,cc,b,t,policy=key
        entry=dict(hierarchy_behavior=hierarchy_behavior(state["config"],cc),hierarchy_layout=hierarchy_layout(state["config"],cc),mode=mode,workload=w,range=length,cc=cc,bucket=b,threads=t,wait_policy=policy,repetitions=len(rows))
        metrics={"throughput":[float(r["throughput_committed_txn_s"]) for r in rows],
                 "abort_pct":[100*int(r["aborted"])/max(1,int(r["transactions"])) for r in rows],
                 "unfinished_pct":[100*int(r.get("logical_unfinished") or 0)/max(1,int(r.get("logical_started") or 0)) for r in rows],
                 "wait_ms_per_logical":[int(r.get("lock_wait_total_ns") or 0)/max(1,int(r.get("logical_started") or 0))/1e6 for r in rows],
                 "retries_per_logical":[int(r.get("retries") or 0)/max(1,int(r.get("logical_started") or 0)) for r in rows],
                 "completed_p99_ms":[int(r["logical_completion_p99_ns"])/1e6 for r in rows if int(r.get("logical_completion_count") or 0)>0]}
        for name,values in metrics.items():
            for agg,fn in (("mean",statistics.mean),("min",min),("max",max)):
                entry[name+"_"+agg]=fn(values) if values else None
        entry["no_progress_repetitions"]=sum(int(r["committed"])==0 for r in rows)
        entry["latency_samples"]=sum(int(r.get("logical_completion_count") or 0) for r in rows)
        summary.append(entry)
    with (out/"summary.csv").open("w",newline="") as f:
        writer=csv.DictWriter(f,fieldnames=list(summary[0]));writer.writeheader();writer.writerows(summary)
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    figures=[]
    for mode,w,length in sorted({(r["mode"],r["workload"],r["range"]) for r in summary}):
        selected=[r for r in summary if (r["mode"],r["workload"],r["range"])==(mode,w,length)]
        fig,axes=plt.subplots(2,2,figsize=(13,8),layout="constrained")
        metrics=[("throughput","Committed transactions / s"),("wait_ms_per_logical","Lock wait ms / started logical txn"),
                 ("retries_per_logical","Retries / started logical txn"),("completed_p99_ms","Mean of per-run completed logical p99 (ms)")]
        series=collections.defaultdict(list)
        for row in selected:series[(row["cc"],row["bucket"],row["wait_policy"])].append(row)
        for ax,(metric,ylabel) in zip(axes.flat,metrics):
            for (cc,b,policy),points in sorted(series.items()):
                points=sorted((p for p in points if p[metric+"_mean"] is not None),key=lambda p:p["threads"])
                if not points:continue
                label=cc+" / "+hierarchy_behavior(state["config"],cc)+" ["+hierarchy_layout(state["config"],cc)+"]"+(" b="+str(b) if b else "")+" / "+policy
                x=[p["threads"] for p in points];y=[p[metric+"_mean"] for p in points]
                error=[[p[metric+"_mean"]-p[metric+"_min"] for p in points],[p[metric+"_max"]-p[metric+"_mean"] for p in points]]
                ax.errorbar(x,y,yerr=error,marker="o",capsize=3,label=label)
            ax.set_xlabel("Worker threads");ax.set_ylabel(ylabel);ax.grid(alpha=.25)
        axes[0,0].legend(fontsize=7)
        title=mode+" / "+w+" / range="+str(length)
        if state["kind"]!="formal-shared-data":title="SMOKE VALIDATION - NOT FORMAL PERFORMANCE | "+title
        fig.suptitle(title,fontsize=11)
        name=mode+"-"+w+"-range"+str(length)+".png";fig.savefig(out/name,dpi=150);plt.close(fig);figures.append(name)
    text=["# "+("共享数据正式矩阵报告" if state["kind"]=="formal-shared-data" else "运行与绘图入口验证（非正式性能结果）"),"",
          "源码提交：`"+state["source_commit"]+"`；输入：`"+str(Path(args.campaign).resolve())+"`。",
          "本地与跨主机分组绘图；跨主机必须读取共享远端索引和记录，禁止混入旧锁服务结果。", "",
          "折线为各次重复的均值，误差线为最小/最大。p99 面板是各次已完成逻辑事务 p99 的均值，不是合并直方图后的总体 p99；无提交样本的点省略，未完成比例和无进展重复保留在 summary.csv。新运行时 none 基线同样经过预热与统一测量驱动，具有完成延迟但没有事务锁等待。旧基线无延迟样本。",
          "原始阻塞、重试、attempt 中止率和未完成比例在原始 CSV；summary.csv 汇总吞吐、等待/启动逻辑事务、重试/启动逻辑事务和已完成逻辑事务的 p99。", "",
          "共享索引的已存在记录更新使用共享结构 latch 和记录短 latch；结构变化仍串行。真实叶模式还持事务级 topology S/X 屏障，插入/删除不能并发；协调后端："+state["config"].get("coordination_backend","tcp-control+rdma-slots")+"；每 worker 协程数："+str(state["config"].get("coroutines",1))+"；预热秒数："+str(state["config"].get("warmup_seconds",0))+"。原生模式的 TCP 用于建链交换 QP/MR 属性、连接存活及故障/断连通知；不承担逐事务或逐锁授权。事务状态、数据和锁槽均为单边 RDMA。此结果不能直接支持与 Sherman/ROLEX 的公平端到端排名。", ""]
    for name in figures:text += ["!["+name+"]("+name+")",""]
    (out/"report.md").write_text("\n".join(text))
    print(out/"report.md")

if __name__=="__main__":main()
