#!/usr/bin/env python3
"""Bounded correctness/diagnostics; does not start a formal matrix."""
import argparse,datetime,hashlib,json,shutil,subprocess
from pathlib import Path

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--build",default="build-shared")
    p.add_argument("--config",default="experiments/configs/griffin_sync_hotspot_correctness.json")
    a=p.parse_args()
    root=Path(__file__).resolve().parents[1];cfg_path=(root/a.config).resolve()
    cfg=json.loads(cfg_path.read_text())
    if (cfg["formal"] is not False or cfg["threads"]!=32 or cfg["execution_mode"]!="threads" or
        cfg["coroutines"]!=1 or not 1<=cfg["hotspot_seconds"]<=10 or not 1<=cfg["stress_seconds"]<=3600 or
        cfg["sync_batch_limits"]!=[1,64]):raise ValueError("unexpected correctness fixture")
    logs=root/"experiments/logs"/("griffin-improvements-"+datetime.datetime.now().strftime("%Y%m%d-%H%M%S-%f"))
    logs.mkdir(parents=True);pins=logs/"binaries";pins.mkdir()
    names=("rrl_precision_hotspot_test","rrl_griffin_soak_test")
    hashes={}
    for name in names:
        source=root/a.build/name;digest=hashlib.sha256(source.read_bytes()).hexdigest()
        shutil.copy2(source,pins/name)
        if hashlib.sha256((pins/name).read_bytes()).hexdigest()!=digest:raise RuntimeError("binary changed")
        hashes[name]=digest
    # Full build-input manifest, including relevant files not yet committed.
    files=[root/"CMakeLists.txt",root/"benchmarks/workload_runner.cpp"]
    for folder in ("include","src","test"):
        files.extend(p for p in (root/folder).rglob("*") if p.suffix in (".cpp",".h"))
    files.extend([Path(__file__).resolve(),cfg_path])
    manifest={str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(set(files))}
    (logs/"source-manifest.json").write_text(json.dumps(manifest,indent=2))
    shutil.copy2(cfg_path,logs/"config.json")
    state=dict(formal=False,status="RUNNING",source_commit=subprocess.check_output(["git","rev-parse","HEAD"],cwd=root,text=True).strip(),
               source_manifest_sha256=hashlib.sha256(json.dumps(manifest,sort_keys=True).encode()).hexdigest(),
               binary_sha256=hashes,cases=[],progress_warnings=[])
    def save():
        tmp=logs/"state.tmp";tmp.write_text(json.dumps(state,indent=2));tmp.replace(logs/"state.json")
    def run(name,args,label,timeout):
        result=subprocess.run([str(pins/name),*map(str,args)],cwd=root,capture_output=True,text=True,timeout=timeout)
        (logs/(label+".stdout")).write_text(result.stdout);(logs/(label+".stderr")).write_text(result.stderr)
        if result.returncode:raise RuntimeError((label,result.returncode,result.stderr[-2000:]))
        state["cases"].append(dict(name=label,status="PASS"));save()
        return result.stdout
    save();print(logs,flush=True)
    try:
        rows=[json.loads(line) for line in run(names[0],[cfg["hotspot_seconds"]],"hotspot",180).splitlines()]
        if len(rows)!=9:raise RuntimeError("incomplete hotspot output")
        for row in rows:
            if not row["progress_observed"] or row["workers_without_commit"]:
                state["progress_warnings"].append({k:row[k] for k in
                    ("policy","variant","committed","active_workers","workers_without_commit","retry_exhausted")})
        for limit in cfg["sync_batch_limits"]:
            out=run(names[1],[cfg["stress_seconds"],limit],"stress-"+str(limit),cfg["stress_seconds"]*3+30)
            if not out.startswith("PASS "):raise RuntimeError("missing stress validation")
        lines=["# Griffin 同步与热点正确性探针","","本次非正式矩阵；32 OS线程，关闭协程。短窗口用于检查结果与指标，不形成性能排序。","",
               "| 等待策略 | 实现 | 提交数 | 完成比例 | 重试耗尽 | 无提交worker/活跃worker | 含重试延迟样本数 |",
               "|---|---|---:|---:|---:|---:|---:|"]
        variants=["Precision + B+树","Griffin PL + B+树","Griffin 混合索引"]
        for x in rows:
            lines.append("| {} | {} | {} | {:.4f} | {} | {}/{} | {} |".format(
                x["policy"],variants[x["variant"]],x["committed"],x["completion_fraction"],x["retry_exhausted"],
                x["workers_without_commit"],x["active_workers"],x["logical_latency_samples"]))
        lines+=["","零提交表示该窗口未观察到推进，不能解释为零延迟，也不能把正确性PASS解释成无饥饿。",
                "受控年龄/升级交错要求实际释放后授予并提交；开放窗口保留零提交与不公平现象，不使用sleep或随机退避隐藏。",
                "每个stress输出均验证最终参考键集合、hash/tree一致和诊断计数守恒；limit=1与64是单变量，但短探针不提供正式加速结论。","",
                "来源HEAD: "+state["source_commit"],"构建输入完整指纹: "+state["source_manifest_sha256"],
                "配置、源码清单、固定二进制SHA256及原始输出均位于本目录。"]
        (logs/"report.md").write_text("\n".join(lines)+"\n")
        state["status"]="CORRECTNESS_PASS";save()
        print("CORRECTNESS_PASS; progress warning cases:",len(state["progress_warnings"]),flush=True)
        print(logs/"report.md",flush=True)
    except BaseException as error:
        state.update(status="FAIL",error=repr(error));save();raise
if __name__=="__main__":main()
